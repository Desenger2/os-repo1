#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

int main(int argc, char* argv[]) {
    if (argc != 5) return 1;
    
    HINSTANCE dll = LoadLibrary(argv[1]);
    if (!dll) return 1;
    
    set_key_func set_key = (set_key_func)GetProcAddress(dll, "set_key");
    caesar_func caesar = (caesar_func)GetProcAddress(dll, "caesar");
    
    if (!set_key || !caesar) {
        FreeLibrary(dll);
        return 1;
    }
    
    FILE* in = fopen(argv[3], "rb");
    if (!in) {
        FreeLibrary(dll);
        return 1;
    }
    
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    
    char* buf = malloc(size);
    if (!buf) {
        fclose(in);
        FreeLibrary(dll);
        return 1;
    }
    
    fread(buf, 1, size, in);
    fclose(in);
    
    set_key(argv[2][0]);
    caesar(buf, buf, size);
    
    FILE* out = fopen(argv[4], "wb");
    if (!out) {
        free(buf);
        FreeLibrary(dll);
        return 1;
    }
    
    fwrite(buf, 1, size, out);
    fclose(out);
    
    free(buf);
    FreeLibrary(dll);
    return 0;
}