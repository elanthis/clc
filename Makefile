VERSION = 0.01

CFLAGS = -Wall -g -DHAVE_ZLIB -DCLC_VERSION='"$(VERSION)"'
LFLAGS = -lcurses -ltelnet -lz

all: clc

clc.o: clc.c
	$(CC) $(CFLAGS) -c -o $@ $<

clc: clc.o
	$(CC) -o $@ $< $(LFLAGS)

dist: clc-$(VERSION).tar.gz

clc-$(VERSION).tar.gz: clc.c Makefile README
	mkdir clc-$(VERSION)
	cp -f $^ clc-$(VERSION)
	tar -cf clc-$(VERSION).tar clc-$(VERSION)
	rm -fr clc-$(VERSION)
	gzip -f clc-$(VERSION).tar

clean:
	rm -f clc clc.o clc-$(VERSION).tar.gz
