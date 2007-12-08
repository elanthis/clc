all: clc

clc: clc.c
	$(CC) -Wall -g -o clc clc.c -lcurses

dist: clc-dist.tar.gz

clc-dist.tar.gz: clc.c Makefile README
	mkdir clc-dist
	cp -f $^ clc-dist
	tar -cf clc-dist.tar clc-dist
	rm -fr clc-dist
	gzip -f clc-dist.tar

clean:
	rm -f clc clc-dist.tar.gz
