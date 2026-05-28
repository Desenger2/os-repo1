
CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -Wl,-rpath,. -L. -lcaesar

TARGET = secure_copy
LIB = libcaesar.so
FUSE_TARGET = sfuse
FUSE_PKG = $(shell command -v fusermount3 >/dev/null && pkg-config --exists fuse3 && echo fuse3 || (pkg-config --exists fuse && echo fuse))
FUSE_CFLAGS = $(shell pkg-config --cflags $(FUSE_PKG) 2>/dev/null) -DFUSE_USE_VERSION=$(shell [ "$(FUSE_PKG)" = "fuse3" ] && echo 31 || echo 26)
FUSE_LIBS = $(shell pkg-config --libs $(FUSE_PKG) 2>/dev/null)

all: $(LIB) $(TARGET)

$(LIB): caesar.c caesar.h
	$(CC) -shared -fPIC caesar.c -o $(LIB)

$(TARGET): secure_copy.o $(LIB)
	$(CC) secure_copy.o -o $(TARGET) $(CFLAGS) $(LDFLAGS)

secure_copy.o: secure_copy.c caesar.h
	$(CC) $(CFLAGS) -c secure_copy.c

$(FUSE_TARGET): sfuse.c $(LIB) caesar.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) sfuse.c -o $(FUSE_TARGET) $(LDFLAGS) $(FUSE_LIBS)

fuse: $(LIB) $(FUSE_TARGET)

clean:
	rm -f *.o $(TARGET) $(LIB) $(FUSE_TARGET)
	rm -f disk.img log.txt result_file
	rm -rf in mnt

run_add:
	./$(TARGET) -add -key "secret" -image disk.img f1.txt f2.txt f3.txt f4.txt f5.txt in

run_list:
	./$(TARGET) -list -image disk.img

run_get:
	./$(TARGET) -get -image disk.img -key "secret" -out result_file.txt in/a/b/c/d/d.txt

run_mount:
	mkdir -p mnt
	./$(FUSE_TARGET) -i disk.img -k "secret" mnt -f

run_umount:
	fusermount3 -u mnt || fusermount -u mnt

files:
	printf "Hello 1" > f1.txt
	printf "Hello 2" > f2.txt
	printf "Hello 3" > f3.txt
	printf "Hello 4" > f4.txt
	printf "Hello 5" > f5.txt
	mkdir -p in/a/b/c/d
	printf "level0" > in/root.txt
	printf "level1" > in/a/a.txt
	printf "level2" > in/a/b/b.txt
	printf "level3" > in/a/b/c/c.txt
	printf "level4" > in/a/b/c/d/d.txt