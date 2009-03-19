LIBTELNET = ../libtelnet

CFLAGS = -Wall -g -I$(LIBTELNET) -DHAVE_ZLIB
LFLAGS = -lcurses -L$(LIBTELNET) -ltelnet -lz

all: clc

clc: clc.c $(LIBTELNET)/libtelnet.a
	$(CC) $(CFLAGS) -o clc clc.c $(LFLAGS)

dist: clc-dist.tar.gz

clc-dist.tar.gz: clc.c Makefile README
	mkdir clc-dist
	cp -f $^ clc-dist
	tar -cf clc-dist.tar clc-dist
	rm -fr clc-dist
	gzip -f clc-dist.tar

clean:
	rm -f clc clc-dist.tar.gz
