all: libcaesar.dll test_caesar.exe

libcaesar.dll: caesar.c
	gcc -shared -o $@ $<

test_caesar.exe: test_caesar.c
	gcc -o $@ $<

clean:
	del *.dll *.exe