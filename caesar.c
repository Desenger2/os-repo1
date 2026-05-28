#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "caesar.h"

static unsigned char* key_region = NULL;
static size_t key_region_size = 0;
static size_t key_len = 0;
static pthread_mutex_t key_mutex = PTHREAD_MUTEX_INITIALIZER;

int set_key(const char* key, size_t len) {
    if (key_region != NULL) destroy_key();
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t region = ((len + (size_t)ps - 1) / (size_t)ps) * (size_t)ps;
    if (region == 0) region = (size_t)ps;
    key_region = mmap(NULL, region, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (key_region == MAP_FAILED) {
        key_region = NULL;
        return -1;
    }
    key_region_size = region;
    key_len = len;
    memcpy(key_region, key, len);
    if (mprotect(key_region, key_region_size, PROT_NONE) == -1) {
        munmap(key_region, key_region_size);
        key_region = NULL;
        key_region_size = 0;
        key_len = 0;
        return -1;
    }
    return 0;
}

void destroy_key(void) {
    if (key_region == NULL) return;
    if (mprotect(key_region, key_region_size, PROT_READ | PROT_WRITE) == 0) {
        volatile unsigned char* p = key_region;
        for (size_t i = 0; i < key_len; i++) p[i] = 0;
    }
    munmap(key_region, key_region_size);
    key_region = NULL;
    key_region_size = 0;
    key_len = 0;
}

char* get_key_ptr(void) {
    return (char*)key_region;
}

/* Layout of the protected per-thread region:
   [0..255]   S-box (256 bytes)
   [256]      index i
   [257]      index j
   The whole region is kept PROT_NONE between chunks and opened
   only for the duration of KSA (init) and each encryption chunk. */
#define SBOX_OFF 0
#define IDX_I_OFF 256
#define IDX_J_OFF 257
#define STATE_BYTES 258

struct rc4_state {
    unsigned char* region;
    size_t region_size;
};

rc4_state* rc4_init(const unsigned char* salt, size_t salt_len) {
    rc4_state* st = malloc(sizeof(rc4_state));
    if (st == NULL) return NULL;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    st->region_size = ((STATE_BYTES + (size_t)ps - 1) / (size_t)ps) * (size_t)ps;
    st->region = mmap(NULL, st->region_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st->region == MAP_FAILED) {
        free(st);
        return NULL;
    }

    unsigned char* S = st->region + SBOX_OFF;
    for (int i = 0; i < 256; i++) S[i] = (unsigned char)i;

    size_t composite_len = key_len + salt_len;
    if (composite_len == 0) {
        munmap(st->region, st->region_size);
        free(st);
        return NULL;
    }

    pthread_mutex_lock(&key_mutex);
    if (mprotect(key_region, key_region_size, PROT_READ) == -1) {
        pthread_mutex_unlock(&key_mutex);
        munmap(st->region, st->region_size);
        free(st);
        return NULL;
    }
    unsigned int j = 0;
    for (int i = 0; i < 256; i++) {
        size_t idx = (size_t)i % composite_len;
        unsigned char kb = (idx < key_len) ? key_region[idx] : salt[idx - key_len];
        j = (j + S[i] + kb) & 0xFF;
        unsigned char t = S[i]; S[i] = S[j]; S[j] = t;
    }
    if (mprotect(key_region, key_region_size, PROT_NONE) == -1) {
        pthread_mutex_unlock(&key_mutex);
        volatile unsigned char* vS = S;
        for (int i = 0; i < 256; i++) vS[i] = 0;
        munmap(st->region, st->region_size);
        free(st);
        return NULL;
    }
    pthread_mutex_unlock(&key_mutex);

    st->region[IDX_I_OFF] = 0;
    st->region[IDX_J_OFF] = 0;

    if (mprotect(st->region, st->region_size, PROT_NONE) == -1) {
        volatile unsigned char* vr = st->region;
        for (size_t i = 0; i < STATE_BYTES; i++) vr[i] = 0;
        munmap(st->region, st->region_size);
        free(st);
        return NULL;
    }
    return st;
}

int rc4_crypt_chunk(rc4_state* st, const unsigned char* in,
                    unsigned char* out, size_t len) {
    if (st == NULL) return -1;
    if (mprotect(st->region, st->region_size, PROT_READ | PROT_WRITE) == -1) {
        return -1;
    }
    unsigned char* S = st->region + SBOX_OFF;
    unsigned int a = st->region[IDX_I_OFF];
    unsigned int b = st->region[IDX_J_OFF];
    for (size_t k = 0; k < len; k++) {
        a = (a + 1) & 0xFF;
        b = (b + S[a]) & 0xFF;
        unsigned char t = S[a]; S[a] = S[b]; S[b] = t;
        out[k] = in[k] ^ S[(S[a] + S[b]) & 0xFF];
    }
    st->region[IDX_I_OFF] = (unsigned char)a;
    st->region[IDX_J_OFF] = (unsigned char)b;
    if (mprotect(st->region, st->region_size, PROT_NONE) == -1) {
        return -1;
    }
    return 0;
}

void rc4_free(rc4_state* st) {
    if (st == NULL) return;
    if (mprotect(st->region, st->region_size, PROT_READ | PROT_WRITE) == 0) {
        volatile unsigned char* vr = st->region;
        for (size_t i = 0; i < STATE_BYTES; i++) vr[i] = 0;
    }
    munmap(st->region, st->region_size);
    free(st);
}