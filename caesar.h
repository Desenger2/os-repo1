#ifndef CAESAR_H
#define CAESAR_H

#include <stddef.h>

void set_key(char key);
void caesar(void* src, void* dst, int len);

#endif