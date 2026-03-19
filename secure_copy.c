#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#include "caesar.h"

#define NUM_THREADS 3
#define TIMEOUT_SEC 5

pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int files_copied = 0;
int total_files = 0;
char** global_file_list = NULL;
char* global_output_dir = NULL;
FILE* log_file = NULL;

struct thread_data {
    int thread_num;
    pthread_t thread_id;
};

void write_log(const char* filename, const char* status, double elapsed_time, pthread_t thread_id) {
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
            printf("Possible deadlock: thread waiting for counter mutex >%d seconds\n", TIMEOUT_SEC);
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

void* process_files(void* arg) {
    struct thread_data* data = (struct thread_data*)arg;
    pthread_t thread_id = pthread_self();
    data->thread_id = thread_id;
    
    while (1) {
        int file_index = get_next_file_index();
        if (file_index >= total_files) {
            break;
        }
        
        char* input_file = global_file_list[file_index];
        clock_t start_time = clock();
        char status[64] = "success";
        
        char output_path[512];
        const char* filename = strrchr(input_file, '/');
        if (filename == NULL) {
            filename = input_file;
        } else {
            filename++;
        }
        
        snprintf(output_path, sizeof(output_path), "%s/%s", global_output_dir, filename);
        
        FILE* in = fopen(input_file, "rb");
        if (in == NULL) {
            snprintf(status, sizeof(status), "error: cannot open input file");
            write_log(input_file, status, 0, thread_id);
            continue;
        }
        
        fseek(in, 0, SEEK_END);
        long file_size = ftell(in);
        fseek(in, 0, SEEK_SET);
        
        if (file_size <= 0) {
            snprintf(status, sizeof(status), "error: empty file or invalid size");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            continue;
        }
        
        char* buffer = malloc(file_size);
        char* encrypted = malloc(file_size);
        
        if (buffer == NULL || encrypted == NULL) {
            snprintf(status, sizeof(status), "error: cannot allocate memory");
            write_log(input_file, status, 0, thread_id);
            fclose(in);
            free(buffer);
            free(encrypted);
            continue;
        }
        
        size_t bytes_read = fread(buffer, 1, file_size, in);
        fclose(in);
        
        if (bytes_read != file_size) {
            snprintf(status, sizeof(status), "error: cannot read file");
            write_log(input_file, status, 0, thread_id);
            free(buffer);
            free(encrypted);
            continue;
        }
        
        caesar(buffer, encrypted, file_size);
        
        FILE* out = fopen(output_path, "wb");
        if (out == NULL) {
            snprintf(status, sizeof(status), "error: cannot create output file");
            write_log(input_file, status, 0, thread_id);
            free(buffer);
            free(encrypted);
            continue;
        }
        
        size_t bytes_written = fwrite(encrypted, 1, file_size, out);
        fclose(out);
        
        if (bytes_written != file_size) {
            snprintf(status, sizeof(status), "error: cannot write file");
            write_log(input_file, status, 0, thread_id);
            free(buffer);
            free(encrypted);
            continue;
        }
        
        free(buffer);
        free(encrypted);
        
        clock_t end_time = clock();
        double elapsed = (double)(end_time - start_time) / CLOCKS_PER_SEC;
        
        write_log(input_file, status, elapsed, thread_id);
    }
    
    free(data);
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s file1 file2 ... output_dir key\n", argv[0]);
        return 1;
    }
    
    char key = argv[argc - 1][0];
    set_key(key); 
    
    global_output_dir = argv[argc - 2];
    
    struct stat st = {0};
    if (stat(global_output_dir, &st) == -1) {
        mkdir(global_output_dir, 0700);
    }
    
    log_file = fopen("log.txt", "a");
    if (log_file == NULL) {
        printf("Error: cannot create log.txt\n");
        return 1;
    }
    
    total_files = argc - 3;
    global_file_list = &argv[1];
    files_copied = 0;
    
    pthread_t threads[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        struct thread_data* data = malloc(sizeof(struct thread_data));
        data->thread_num = i + 1;
        
        if (pthread_create(&threads[i], NULL, process_files, data) != 0) {
            printf("Error: cannot create thread %d\n", i + 1);
            free(data);
        }
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    fclose(log_file);
    return 0;
}