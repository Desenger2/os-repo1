CC       = gcc
CFLAGS   = -Wall -pthread
LIB_SRC  = caesar.c
LIB      = libcaesar.so
BIN      = secure_copy

all: $(LIB) $(BIN)

$(LIB): $(LIB_SRC)
	$(CC) -fPIC -shared -o $@ $<

$(BIN): secure_copy.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lrt

clean:
	rm -f $(LIB) $(BIN) large_input.txt enc.txt dec.txt

test: all
	dd if=/dev/urandom of=large_input.txt bs=15M count=10 2>/dev/null
	./$(BIN) large_input.txt enc.txt K
	./$(BIN) enc.txt dec.txt K
	diff large_input.txt dec.txt && echo "Test PASSED" || echo "Test FAILED"

.PHONY: all clean test