CC ?= gcc
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wno-unused-function -fPIC

all: liblodepng.a

liblodepng.a: lodepng.o
	$(AR) rcs $@ $^

lodepng.o: lodepng.c lodepng.h
	$(CC) $(CFLAGS) -c lodepng.c -o $@

clean:
	$(RM) liblodepng.a lodepng.o

.PHONY: all clean
