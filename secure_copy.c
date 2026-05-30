#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>

#include "caesar.h"

#define MAX_THREADS 5
#define SALT_SIZE 16
#define TIMEOUT_SEC 5

#define MAX_FILE_SIZE 0xFFFFFFFFULL
#define MAX_NAME_LEN  0xFFFFFFFFULL

#ifndef CHUNK_SIZE
#define CHUNK_SIZE (4 * 1024 * 1024)
#endif

pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t error_mutex = PTHREAD_MUTEX_INITIALIZER;

int files_copied = 0;
int total_files = 0;
int global_had_error = 0;
char** global_file_list = NULL;
char** global_name_list = NULL;
long long* global_offsets = NULL;
long long* global_sizes = NULL;
const char* global_image_path = NULL;
int global_image_fd = -1;
FILE* log_file = NULL;

struct thread_data {
    int thread_num;
    pthread_t thread_id;
};

void wipe_str(char* s) {
    if (s == NULL) return;
    volatile char* p = s;
    while (*p) *p++ = 0;
}

void write_log(const char* filename, const char* status, double elapsed_time, pthread_t thread_id) {
    if (log_file == NULL) return;
    time_t raw_time;
    struct tm* time_info;
    char time_str[80];

    time(&raw_time);
    time_info = localtime(&raw_time);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", time_info);

    pthread_mutex_lock(&log_mutex);
    fprintf(log_file, "[%s] PID: %lu, File: %s, Status: %s, Time: %.3f sec\n",
            time_str, (unsigned long)thread_id, filename, status, elapsed_time);
    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

int get_next_file_index() {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += TIMEOUT_SEC;

    int lock_result = pthread_mutex_timedlock(&counter_mutex, &timeout);
    if (lock_result != 0) {
        if (lock_result == ETIMEDOUT) {
            fprintf(stderr, "Possible deadlock: thread waiting for counter mutex >%d seconds\n", TIMEOUT_SEC);
        }
        exit(1);
    }

    int index = files_copied;
    if (index < total_files) {
        files_copied++;
    }
    pthread_mutex_unlock(&counter_mutex);
    return index;
}

int generate_salt(unsigned char* salt, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, salt + off, len - off);
        if (n <= 0) { close(fd); return -1; }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

int read_u32_le(FILE* f, uint32_t* v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

FILE* open_image_ro(const char* image) {
    struct stat st;
    if (stat(image, &st) == -1) {
        fprintf(stderr, "Error: cannot open image '%s': %s\n", image, strerror(errno));
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file\n", image);
        return NULL;
    }
    FILE* f = fopen(image, "rb");
    if (f == NULL) {
        fprintf(stderr, "Error: cannot open image '%s': %s\n", image, strerror(errno));
        return NULL;
    }
    return f;
}

int image_contains_name(const char* image_path, const char* target_name) {
    FILE* f = fopen(image_path, "rb");
    if (f == NULL) return 0;
    size_t tlen = strlen(target_name);
    int found = 0;
    while (1) {
        uint32_t fsize, nlen;
        if (read_u32_le(f, &fsize) == -1) break;
        if (read_u32_le(f, &nlen) == -1) break;
        if (fseek(f, SALT_SIZE, SEEK_CUR) != 0) break;
        if (nlen == tlen) {
            char* name = malloc((size_t)nlen + 1);
            if (name == NULL) break;
            if (fread(name, 1, nlen, f) != nlen) { free(name); break; }
            name[nlen] = '\0';
            if (strcmp(name, target_name) == 0) {
                free(name);
                found = 1;
                break;
            }
            free(name);
        } else {
            if (fseek(f, nlen, SEEK_CUR) != 0) break;
        }
        if (fseek(f, fsize, SEEK_CUR) != 0) break;
    }
    fclose(f);
    return found;
}

void mark_error(void) {
    pthread_mutex_lock(&error_mutex);
    global_had_error = 1;
    pthread_mutex_unlock(&error_mutex);
}

void* process_files(void* arg) {
    struct thread_data* data = (struct thread_data*)arg;
    pthread_t thread_id = pthread_self();
    data->thread_id = thread_id;

    unsigned char* inbuf = malloc(CHUNK_SIZE);
    unsigned char* outbuf = malloc(CHUNK_SIZE);
    if (inbuf == NULL || outbuf == NULL) {
        free(inbuf); free(outbuf); free(data);
        mark_error();
        return NULL;
    }

    while (1) {
        int file_index = get_next_file_index();
        if (file_index >= total_files) {
            break;
        }

        char* input_file = global_file_list[file_index];
        char* stored_name = global_name_list[file_index];
        long long base = global_offsets[file_index];
        off_t file_size = (off_t)global_sizes[file_index];
        clock_t start_time = clock();
        char status[64] = "success";

        FILE* in = fopen(input_file, "rb");
        if (in == NULL) {
            snprintf(status, sizeof(status), "error: cannot open input file");
            write_log(input_file, status, 0, thread_id);
            mark_error();
            continue;
        }

        unsigned char salt[SALT_SIZE];
        if (generate_salt(salt, SALT_SIZE) == -1) {
            snprintf(status, sizeof(status), "error: cannot generate salt");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            mark_error();
            continue;
        }

        size_t name_len = strlen(stored_name);
        size_t header_len = 8 + SALT_SIZE + name_len;
        unsigned char* header = malloc(header_len);
        if (header == NULL) {
            snprintf(status, sizeof(status), "error: cannot allocate memory");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            mark_error();
            continue;
        }
        uint32_t fs32 = (uint32_t)file_size;
        uint32_t nl32 = (uint32_t)name_len;
        header[0] = fs32 & 0xFF; header[1] = (fs32 >> 8) & 0xFF;
        header[2] = (fs32 >> 16) & 0xFF; header[3] = (fs32 >> 24) & 0xFF;
        header[4] = nl32 & 0xFF; header[5] = (nl32 >> 8) & 0xFF;
        header[6] = (nl32 >> 16) & 0xFF; header[7] = (nl32 >> 24) & 0xFF;
        memcpy(header + 8, salt, SALT_SIZE);
        memcpy(header + 8 + SALT_SIZE, stored_name, name_len);

        int werr = 0;
        if (pwrite(global_image_fd, header, header_len, base) != (ssize_t)header_len) werr = 1;
        free(header);
        if (werr) {
            snprintf(status, sizeof(status), "error: cannot write image header");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            mark_error();
            continue;
        }

        rc4_state* st = rc4_init(salt, SALT_SIZE);
        if (st == NULL) {
            snprintf(status, sizeof(status), "error: cannot init cipher");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            mark_error();
            continue;
        }

        off_t data_base = base + (off_t)header_len;
        off_t written = 0;
        int io_error = 0;
        while (written < file_size) {
            size_t want = CHUNK_SIZE;
            if ((off_t)want > file_size - written) want = (size_t)(file_size - written);
            size_t got = fread(inbuf, 1, want, in);
            if (got != want) { io_error = 1; break; }
            if (rc4_crypt_chunk(st, inbuf, outbuf, got) != 0) { io_error = 1; break; }
            if (pwrite(global_image_fd, outbuf, got, data_base + written) != (ssize_t)got) { io_error = 1; break; }
            written += (off_t)got;
        }
        rc4_free(st);
        fclose(in);

        if (io_error) {
            snprintf(status, sizeof(status), "error: cannot read/write file");
            write_log(input_file, status, 0, thread_id);
            mark_error();
            continue;
        }

        clock_t end_time = clock();
        double elapsed = (double)(end_time - start_time) / CLOCKS_PER_SEC;
        write_log(input_file, status, elapsed, thread_id);
    }

    free(inbuf);
    free(outbuf);
    free(data);
    return NULL;
}

double run_threads(int num_threads) {
    pthread_t threads[num_threads];
    files_copied = 0;

    struct timespec s, e;
    clock_gettime(CLOCK_MONOTONIC, &s);

    for (int i = 0; i < num_threads; i++) {
        struct thread_data* data = malloc(sizeof(struct thread_data));
        if (data == NULL) continue;
        data->thread_num = i + 1;
        if (pthread_create(&threads[i], NULL, process_files, data) != 0) {
            fprintf(stderr, "Error: cannot create thread %d\n", i + 1);
            free(data);
            threads[i] = 0;
        }
    }

    for (int i = 0; i < num_threads; i++) {
        if (threads[i] != 0) pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &e);
    return (e.tv_sec - s.tv_sec) + (e.tv_nsec - s.tv_nsec) / 1e9;
}

int append_entry(char*** files, char*** names, int* count, int* cap,
                 const char* fs_path, const char* stored_name) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        char** nf = realloc(*files, new_cap * sizeof(char*));
        char** nn = realloc(*names, new_cap * sizeof(char*));
        if (nf == NULL || nn == NULL) return -1;
        *files = nf;
        *names = nn;
        *cap = new_cap;
    }
    (*files)[*count] = strdup(fs_path);
    (*names)[*count] = strdup(stored_name);
    if ((*files)[*count] == NULL || (*names)[*count] == NULL) return -1;
    (*count)++;
    return 0;
}

int collect_dir(const char* fs_dir, const char* rel_prefix,
                char*** files, char*** names, int* count, int* cap) {
    DIR* d = opendir(fs_dir);
    if (d == NULL) {
        fprintf(stderr, "Error: cannot open directory '%s': %s\n", fs_dir, strerror(errno));
        return -1;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child_fs[4096];
        char child_rel[4096];
        snprintf(child_fs, sizeof(child_fs), "%s/%s", fs_dir, ent->d_name);
        snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, ent->d_name);
        struct stat st;
        if (lstat(child_fs, &st) == -1) {
            fprintf(stderr, "Error: cannot stat '%s': %s\n", child_fs, strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (collect_dir(child_fs, child_rel, files, names, count, cap) == -1) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (append_entry(files, names, count, cap, child_fs, child_rel) == -1) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

int cmd_add(int argc, char* argv[]) {
    const char* key = NULL;
    const char* image = NULL;
    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
            key = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
            image = argv[i + 1]; i += 2;
        } else {
            break;
        }
    }
    if (key == NULL || image == NULL || i >= argc) {
        fprintf(stderr, "Usage: %s -add -key KEY -image IMAGE file_or_dir ...\n", argv[0]);
        return 1;
    }

    char** files = NULL;
    char** names = NULL;
    int count = 0;
    int cap = 0;

    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == -1) {
            fprintf(stderr, "Error: cannot stat '%s': %s\n", argv[i], strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char prefix[4096];
            char* canon = realpath(argv[i], NULL);
            if (canon != NULL) {
                size_t cl = strlen(canon);
                while (cl > 1 && canon[cl - 1] == '/') canon[--cl] = '\0';
                const char* base = strrchr(canon, '/');
                base = (base == NULL) ? canon : (base + 1);
                if (*base == '\0') prefix[0] = '\0';
                else snprintf(prefix, sizeof(prefix), "%s", base);
                free(canon);
            } else {
                snprintf(prefix, sizeof(prefix), "%s", argv[i]);
                size_t plen = strlen(prefix);
                while (plen > 1 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
                const char* base = strrchr(prefix, '/');
                if (base != NULL) memmove(prefix, base + 1, strlen(base + 1) + 1);
            }
            if (collect_dir(argv[i], prefix, &files, &names, &count, &cap) == -1) {
                fprintf(stderr, "Error: failed to collect '%s'\n", argv[i]);
            }
        } else if (S_ISREG(st.st_mode)) {
            const char* base = strrchr(argv[i], '/');
            base = (base == NULL) ? argv[i] : (base + 1);
            if (append_entry(&files, &names, &count, &cap, argv[i], base) == -1) {
                fprintf(stderr, "Error: cannot append '%s'\n", argv[i]);
            }
        } else {
            fprintf(stderr, "Warning: skipping '%s' (not a regular file or directory)\n", argv[i]);
        }
    }

    if (count == 0) {
        fprintf(stderr, "Error: no files to add\n");
        free(files); free(names);
        return 1;
    }

    log_file = fopen("log.txt", "a");

    int wcount = 0;
    for (int k = 0; k < count; k++) {
        int skip = 0;
        const char* reason = NULL;

        struct stat fst;
        if (stat(files[k], &fst) == 0 && S_ISREG(fst.st_mode)) {
            if ((unsigned long long)fst.st_size > MAX_FILE_SIZE) {
                skip = 1;
                reason = "skipped: file too large (max 4 GiB)";
                fprintf(stderr, "Error: '%s' exceeds maximum file size (4 GiB), skipped\n", files[k]);
            }
        }
        if (!skip && (unsigned long long)strlen(names[k]) > MAX_NAME_LEN) {
            skip = 1;
            reason = "skipped: name too long";
            fprintf(stderr, "Error: name for '%s' too long, skipped\n", files[k]);
        }

        int dup = 0;
        if (!skip) {
            if (image_contains_name(image, names[k])) dup = 1;
            for (int m = 0; !dup && m < wcount; m++) {
                if (strcmp(names[m], names[k]) == 0) dup = 1;
            }
            if (dup) reason = "skipped: duplicate name";
        }

        if (skip || dup) {
            if (log_file != NULL && reason != NULL) {
                pthread_mutex_lock(&log_mutex);
                fprintf(log_file, "File: %s, Status: %s\n", files[k], reason);
                fflush(log_file);
                pthread_mutex_unlock(&log_mutex);
            }
            free(files[k]);
            free(names[k]);
            files[k] = NULL;
            names[k] = NULL;
        } else {
            files[wcount] = files[k];
            names[wcount] = names[k];
            wcount++;
        }
    }
    count = wcount;

    if (count == 0) {
        fprintf(stderr, "Error: no files to add (all skipped)\n");
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }

    int fd = open(image, O_RDWR | O_CREAT, 0600);
    if (fd == -1) {
        fprintf(stderr, "Error: cannot open image '%s': %s\n", image, strerror(errno));
        for (int k = 0; k < count; k++) { free(files[k]); free(names[k]); }
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }
    struct stat img_st;
    if (fstat(fd, &img_st) == -1 || !S_ISREG(img_st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file\n", image);
        close(fd);
        for (int k = 0; k < count; k++) { free(files[k]); free(names[k]); }
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }
    off_t image_end = lseek(fd, 0, SEEK_END);
    if (image_end == -1) {
        fprintf(stderr, "Error: cannot seek image: %s\n", strerror(errno));
        close(fd);
        for (int k = 0; k < count; k++) { free(files[k]); free(names[k]); }
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }

    long long* offsets = malloc((size_t)count * sizeof(long long));
    long long* sizes = malloc((size_t)count * sizeof(long long));
    if (offsets == NULL || sizes == NULL) {
        fprintf(stderr, "Error: cannot allocate offsets\n");
        free(offsets); free(sizes);
        close(fd);
        for (int k = 0; k < count; k++) { free(files[k]); free(names[k]); }
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }
    long long running = (long long)image_end;
    for (int k = 0; k < count; k++) {
        struct stat fst;
        off_t fsz = 0;
        if (stat(files[k], &fst) == 0 && S_ISREG(fst.st_mode)) fsz = fst.st_size;
        sizes[k] = (long long)fsz;
        offsets[k] = running;
        running += 8 + SALT_SIZE + (long long)strlen(names[k]) + (long long)fsz;
    }

    global_file_list = files;
    global_name_list = names;
    global_offsets = offsets;
    global_sizes = sizes;
    total_files = count;
    global_image_path = image;
    global_image_fd = fd;

    if (set_key(key, strlen(key)) == -1) {
        fprintf(stderr, "Error: cannot initialize protected key\n");
        close(fd);
        free(offsets); free(sizes);
        for (int k = 0; k < count; k++) { free(files[k]); free(names[k]); }
        free(files); free(names);
        if (log_file != NULL) fclose(log_file);
        return 1;
    }
    wipe_str((char*)key);
    key = NULL;

    global_had_error = 0;

    int threads = (count < 2) ? 1 : MAX_THREADS;
    double elapsed = run_threads(threads);

    int rolled_back = 0;
    if (global_had_error) {
        if (ftruncate(fd, image_end) == 0) {
            rolled_back = 1;
        }
        fprintf(stderr, "Error: one or more files failed; image rolled back to previous state\n");
        if (log_file != NULL) {
            pthread_mutex_lock(&log_mutex);
            fprintf(log_file, "Status: transaction failed, image %s\n",
                    rolled_back ? "rolled back" : "ROLLBACK FAILED");
            fflush(log_file);
            pthread_mutex_unlock(&log_mutex);
        }
    }

    double avg = count ? elapsed / count : 0;
    printf("mode: %s\n", threads == 1 ? "sequential" : "parallel");
    printf("files: %d\n", count);
    printf("total: %.3f sec\n", elapsed);
    printf("avg  : %.6f sec/file\n", avg);

    close(fd);
    free(offsets);
    free(sizes);
    for (int k = 0; k < count; k++) {
        free(files[k]);
        free(names[k]);
    }
    free(files);
    free(names);

    destroy_key();
    if (log_file != NULL) fclose(log_file);
    return global_had_error ? 1 : 0;
}

int cmd_list(int argc, char* argv[]) {
    const char* image = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
            image = argv[i + 1]; i++;
        }
    }
    if (image == NULL) {
        fprintf(stderr, "Usage: %s -list -image IMAGE\n", argv[0]);
        return 1;
    }
    FILE* f = open_image_ro(image);
    if (f == NULL) {
        return 1;
    }

    struct entry { char* name; uint32_t size; };
    struct entry* arr = NULL;
    int n = 0, cap = 0;
    int err = 0;

    while (1) {
        uint32_t fsize, nlen;
        unsigned char salt[SALT_SIZE];
        if (read_u32_le(f, &fsize) == -1) break;
        if (read_u32_le(f, &nlen) == -1) { err = 1; break; }
        if (fread(salt, 1, SALT_SIZE, f) != SALT_SIZE) { err = 1; break; }
        char* name = malloc((size_t)nlen + 1);
        if (name == NULL) { err = 1; break; }
        if (fread(name, 1, nlen, f) != nlen) { free(name); err = 1; break; }
        name[nlen] = '\0';
        if (fseek(f, fsize, SEEK_CUR) != 0) { free(name); err = 1; break; }

        if (n >= cap) {
            int new_cap = (cap == 0) ? 16 : (cap * 2);
            struct entry* na = realloc(arr, new_cap * sizeof(struct entry));
            if (na == NULL) { free(name); err = 1; break; }
            arr = na; cap = new_cap;
        }
        arr[n].name = name;
        arr[n].size = fsize;
        n++;
    }
    fclose(f);

    if (err) {
        fprintf(stderr, "Warning: image may be truncated or corrupted\n");
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(arr[i].name, arr[j].name) > 0) {
                struct entry t = arr[i]; arr[i] = arr[j]; arr[j] = t;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        printf("%s\t%u\n", arr[i].name, arr[i].size);
        free(arr[i].name);
    }
    free(arr);
    return 0;
}

int cmd_get(int argc, char* argv[]) {
    const char* image = NULL;
    const char* key = NULL;
    const char* out_path = NULL;
    const char* target_name = NULL;
    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
            image = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
            key = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-out") == 0 && i + 1 < argc) {
            out_path = argv[i + 1]; i += 2;
        } else {
            target_name = argv[i]; i++;
        }
    }
    if (image == NULL || key == NULL || out_path == NULL || target_name == NULL) {
        fprintf(stderr, "Usage: %s -get -image IMAGE -key KEY -out OUT file_name\n", argv[0]);
        return 1;
    }

    FILE* f = open_image_ro(image);
    if (f == NULL) {
        return 1;
    }

    if (set_key(key, strlen(key)) == -1) {
        fprintf(stderr, "Error: cannot initialize protected key\n");
        fclose(f);
        return 1;
    }
    wipe_str((char*)key);
    key = NULL;
    int rc = 1;
    int found = 0;

    while (1) {
        uint32_t fsize, nlen;
        unsigned char salt[SALT_SIZE];
        if (read_u32_le(f, &fsize) == -1) break;
        if (read_u32_le(f, &nlen) == -1) break;
        if (fread(salt, 1, SALT_SIZE, f) != SALT_SIZE) break;
        char* name = malloc((size_t)nlen + 1);
        if (name == NULL) break;
        if (fread(name, 1, nlen, f) != nlen) { free(name); break; }
        name[nlen] = '\0';

        if (strcmp(name, target_name) == 0) {
            free(name);
            found = 1;
            FILE* out = fopen(out_path, "wb");
            if (out == NULL) {
                fprintf(stderr, "Error: cannot create output '%s': %s\n", out_path, strerror(errno));
                rc = 1;
                break;
            }
            rc4_state* st = rc4_init(salt, SALT_SIZE);
            if (st == NULL) {
                fprintf(stderr, "Error: cannot init cipher\n");
                fclose(out);
                rc = 1;
                break;
            }
            unsigned char* cbuf = malloc(CHUNK_SIZE);
            unsigned char* pbuf = malloc(CHUNK_SIZE);
            if (cbuf == NULL || pbuf == NULL) {
                free(cbuf); free(pbuf);
                rc4_free(st);
                fclose(out);
                fprintf(stderr, "Error: cannot allocate memory\n");
                rc = 1;
                break;
            }
            uint32_t remaining = fsize;
            int io_error = 0;
            while (remaining > 0) {
                size_t want = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
                if (fread(cbuf, 1, want, f) != want) { io_error = 1; break; }
                if (rc4_crypt_chunk(st, cbuf, pbuf, want) != 0) { io_error = 1; break; }
                if (fwrite(pbuf, 1, want, out) != want) { io_error = 1; break; }
                remaining -= (uint32_t)want;
            }
            volatile unsigned char* vp = pbuf;
            for (size_t z = 0; z < CHUNK_SIZE; z++) vp[z] = 0;
            free(cbuf); free(pbuf);
            rc4_free(st);
            fclose(out);
            if (io_error) {
                fprintf(stderr, "Error: cannot read/decrypt/write content\n");
                rc = 1;
            } else {
                rc = 0;
            }
            break;
        } else {
            if (fseek(f, fsize, SEEK_CUR) != 0) { free(name); break; }
            free(name);
        }
    }
    fclose(f);
    destroy_key();
    if (!found) {
        fprintf(stderr, "Error: file '%s' not found in image\n", target_name);
    }
    return rc;
}

void sigsegv_handler(int sig, siginfo_t* info, void* context) {
    (void)sig; (void)info; (void)context;
    const char msg[] = "Security error: attempt to modify protected key memory\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(2);
}

void demo_write_attempt(void) {
    char* k = get_key_ptr();
    if (k == NULL) return;
    printf("Demonstrating write attempt to protected memory...\n");
    fflush(stdout);
    k[0] = 'Z';
}

int cmd_demo(int argc, char* argv[]) {
    const char* key = NULL;
    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
            key = argv[i + 1]; i += 2;
        } else {
            break;
        }
    }
    if (key == NULL) {
        fprintf(stderr, "Usage: %s -demo-write -key KEY\n", argv[0]);
        return 1;
    }

    if (set_key(key, strlen(key)) == -1) {
        fprintf(stderr, "Error: cannot initialize protected key\n");
        return 1;
    }
    wipe_str((char*)key);
    key = NULL;

    struct sigaction sa;
    sa.sa_sigaction = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        perror("sigaction");
        destroy_key();
        return 1;
    }

    demo_write_attempt();

    destroy_key();
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n"
                        "  %s -add -key KEY -image IMAGE file_or_dir ...\n"
                        "  %s -list -image IMAGE\n"
                        "  %s -get -image IMAGE -key KEY -out OUT file_name\n"
                        "  %s -demo-write -key KEY\n",
                        argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "-add") == 0) return cmd_add(argc, argv);
    if (strcmp(argv[1], "-list") == 0) return cmd_list(argc, argv);
    if (strcmp(argv[1], "-get") == 0) return cmd_get(argc, argv);
    if (strcmp(argv[1], "-demo-write") == 0) return cmd_demo(argc, argv);
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}