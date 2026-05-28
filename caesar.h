#ifndef CAESAR_H
#define CAESAR_H

#include <stddef.h>

int  set_key(const char* key, size_t len);
void destroy_key(void);
char* get_key_ptr(void);

typedef struct rc4_state rc4_state;

rc4_state* rc4_init(const unsigned char* salt, size_t salt_len);
int  rc4_crypt_chunk(rc4_state* st, const unsigned char* in,
                     unsigned char* out, size_t len);
void rc4_free(rc4_state* st);

#endif