CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -Wl,-rpath,. -L. -lcaesar
TARGET = secure_copy
LIB_TARGET = libcaesar.so
OBJS = secure_copy.o

all: $(LIB_TARGET) $(TARGET)

$(LIB_TARGET): caesar.c caesar.h
	$(CC) -shared -fPIC caesar.c -o $(LIB_TARGET)

$(TARGET): $(OBJS) $(LIB_TARGET)
	$(CC) $(OBJS) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

secure_copy.o: secure_copy.c caesar.h
	$(CC) $(CFLAGS) -c secure_copy.c -o secure_copy.o

clean:
	rm -f $(OBJS) $(TARGET) $(LIB_TARGET)
	rm -rf outdir/

run:
	./$(TARGET) f1.txt f2.txt f3.txt f4.txt f5.txt outdir/ X

files:
	printf "Hello World 1" > f1.txt
	printf "Hello World 2" > f2.txt
	printf "Hello World 3" > f3.txt
	printf "Hello World 4" > f4.txt
	printf "Hello World 5" > f5.txt
