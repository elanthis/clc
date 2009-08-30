CFLAGS = -Wall -g -DHAVE_ZLIB
LFLAGS = -lcurses -ltelnet -lz

all: clc

clc.o: clc.c
	$(CC) $(CFLAGS) -c -o $@ $<

clc: clc.o
	$(CC) -o $@ $< $(LFLAGS)

dist: clc-dist.tar.gz

clc-dist.tar.gz: clc.c Makefile README
	mkdir clc-dist
	cp -f $^ clc-dist
	tar -cf clc-dist.tar clc-dist
	rm -fr clc-dist
	gzip -f clc-dist.tar

clean:
	rm -f clc clc.o clc-dist.tar.gz
