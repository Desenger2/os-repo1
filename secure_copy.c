#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <dlfcn.h>
#include <mqueue.h>
#include <errno.h>

#define CHUNK_SIZE 4096
#define QUEUE_NAME "/secure_copy_q"
#define QUEUE_DEPTH 4

typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

typedef struct {
    char data[CHUNK_SIZE];
    int  len;
    int  eof;
} Chunk;

static volatile int  keep_running = 1;
static const char   *out_path     = NULL;
static mqd_t         mqd;

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
    Chunk poison = { .len = 0, .eof = 1 };
    mq_send(mqd, (char*)&poison, sizeof(Chunk), 0);
}

typedef struct {
    FILE      *in;
    caesar_fn  caesar;
} ProdArgs;

typedef struct {
    FILE *out;
} ConsArgs;

static void *producer(void *arg) {
    ProdArgs *a = arg;
    Chunk c;
    while (keep_running) {
        memset(&c, 0, sizeof(c));
        c.len = (int)fread(c.data, 1, CHUNK_SIZE, a->in);
        c.eof = feof(a->in);
        if (c.len > 0)
            a->caesar(c.data, c.data, c.len);
        if (c.len > 0 || c.eof) {
            while (mq_send(mqd, (char*)&c, sizeof(Chunk), 0) == -1) {
                if (!keep_running) return NULL;
                if (errno != EINTR) return NULL;
            }
        }
        if (c.eof || c.len == 0)
            break;
    }
    return NULL;
}

static void *consumer(void *arg) {
    ConsArgs *a = arg;
    Chunk c;
    unsigned prio;
    while (keep_running) {
        ssize_t r = mq_receive(mqd, (char*)&c, sizeof(Chunk), &prio);
        if (r == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (c.len > 0)
            fwrite(c.data, 1, c.len, a->out);
        if (c.eof)
            break;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s input output key\n", argv[0]);
        return 1;
    }

    void *lib = dlopen("./libcaesar.so", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    set_key_fn set_key = (set_key_fn)dlsym(lib, "set_key");
    caesar_fn  caesar  = (caesar_fn) dlsym(lib, "caesar");
    if (!set_key || !caesar) { fprintf(stderr, "dlsym failed\n"); dlclose(lib); return 1; }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); dlclose(lib); return 1; }

    out_path = argv[2];
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(argv[2]); fclose(in); dlclose(lib); return 1; }

    set_key(argv[3][0]);

    mq_unlink(QUEUE_NAME);
    struct mq_attr attr = {
        .mq_flags   = 0,
        .mq_maxmsg  = QUEUE_DEPTH,
        .mq_msgsize = sizeof(Chunk),
    };
    mqd = mq_open(QUEUE_NAME, O_CREAT | O_RDWR, 0600, &attr);
    if (mqd == (mqd_t)-1) { perror("mq_open"); fclose(in); fclose(out); dlclose(lib); return 1; }

    struct sigaction sa = { .sa_handler = sig_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    ProdArgs pa = { in, caesar };
    ConsArgs ca = { out };

    pthread_t pt, ct;
    pthread_create(&ct, NULL, consumer, &ca);
    pthread_create(&pt, NULL, producer, &pa);

    pthread_join(pt, NULL);
    pthread_join(ct, NULL);

    fclose(in);
    fclose(out);
    mq_close(mqd);
    mq_unlink(QUEUE_NAME);
    dlclose(lib);

    if (!keep_running) {
        remove(out_path);
        printf("Операция прервана пользователем\n");
        return 1;
    }
    return 0;
}