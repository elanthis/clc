VERSION = 0.02

CFLAGS := -Wall -g -O0
LFLAGS :=

LIBTELNET_CFLAGS := $(shell pkg-config libtelnet --cflags)
LIBTELNET_LFLAGS := $(shell pkg-config libtelnet --libs)

CURSES_CFLAGS :=
CURSES_LFLAGS := -lcurses

CLC_CONFIG := -DCLC_VERSION='"$(VERSION)"'

all: clc

clc.o: clc.c
	$(CC) $(CLC_CONFIG) $(LIBTELNET_CFLAGS) $(CURSES_CFLAGS) $(CFLAGS) -c -o $@ $<

clc: clc.o
	$(CC) -o $@ $< $(LIBTELNET_LFLAGS) $(CURSES_LFLAGS) $(LFLAGS)

dist: clc-$(VERSION).tar.gz

clc-$(VERSION).tar.gz: clc.c Makefile README
	mkdir clc-$(VERSION)
	cp -f $^ clc-$(VERSION)
	tar -cf clc-$(VERSION).tar clc-$(VERSION)
	rm -fr clc-$(VERSION)
	gzip -f clc-$(VERSION).tar

clean:
	rm -f clc clc.o clc-$(VERSION).tar.gz
