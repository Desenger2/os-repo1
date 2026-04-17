CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -Wl,-rpath,. -L. -lcaesar

TARGET = secure_copy
LIB = libcaesar.so

all: $(LIB) $(TARGET)

$(LIB): caesar.c caesar.h
	$(CC) -shared -fPIC caesar.c -o $(LIB)

$(TARGET): secure_copy.o $(LIB)
	$(CC) secure_copy.o -o $(TARGET) $(CFLAGS) $(LDFLAGS)

secure_copy.o: secure_copy.c caesar.h
	$(CC) $(CFLAGS) -c secure_copy.c

clean:
	rm -f *.o $(TARGET) $(LIB)
	rm -rf outdir log.txt

run:
	./$(TARGET) f1.txt f2.txt f3.txt f4.txt f5.txt outdir X

run_seq:
	./$(TARGET) --mode=sequential f1.txt f2.txt f3.txt outdir X

run_par:
	./$(TARGET) --mode=parallel f1.txt f2.txt f3.txt f4.txt f5.txt outdir X

files:
	printf "Hello 1" > f1.txt
	printf "Hello 2" > f2.txt
	printf "Hello 3" > f3.txt
	printf "Hello 4" > f4.txt
	printf "Hello 5" > f5.txt