#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define KEY_SIZE 16

static char* key_ptr = NULL;
static pthread_mutex_t key_mutex = PTHREAD_MUTEX_INITIALIZER;

void set_key(char k) {
    if (key_ptr == NULL) {
        key_ptr = mmap(NULL, KEY_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (key_ptr == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
    } else {
        if (mprotect(key_ptr, KEY_SIZE, PROT_READ | PROT_WRITE) == -1) {
            perror("mprotect");
            exit(1);
        }
    }

    memset(key_ptr, 0, KEY_SIZE);
    memcpy(key_ptr, &k, sizeof(k));

    if (mprotect(key_ptr, KEY_SIZE, PROT_NONE) == -1) {
        perror("mprotect");
        exit(1);
    }
}

void caesar(void* src, void* dst, int len) {
    char* s = src;
    char* d = dst;

    pthread_mutex_lock(&key_mutex);

    if (mprotect(key_ptr, KEY_SIZE, PROT_READ) == -1) {
        perror("mprotect");
        exit(1);
    }

    for (int i = 0; i < len; i++) {
        d[i] = s[i] ^ key_ptr[0];
    }

    if (mprotect(key_ptr, KEY_SIZE, PROT_NONE) == -1) {
        perror("mprotect");
        exit(1);
    }

    pthread_mutex_unlock(&key_mutex);
}

void destroy_key(void) {
    if (key_ptr == NULL) return;
    if (mprotect(key_ptr, KEY_SIZE, PROT_READ | PROT_WRITE) == -1) {
        perror("mprotect");
        return;
    }
    memset(key_ptr, 0, KEY_SIZE);
    munmap(key_ptr, KEY_SIZE);
    key_ptr = NULL;
}

char* get_key_ptr(void) {
    return key_ptr;
}