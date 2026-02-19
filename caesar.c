static char key = 0;

void set_key(char k) {
    key = k;
}

void caesar(void* src, void* dst, int len) {
    char* s = src;
    char* d = dst;
    for (int i = 0; i < len; i++) {
        d[i] = s[i] ^ key;
    }
}