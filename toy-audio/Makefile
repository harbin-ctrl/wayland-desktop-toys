CC ?= gcc
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -fPIC

all: libtoyaudio.a

libtoyaudio.a: toy_audio.o
	$(AR) rcs $@ $^

toy_audio.o: toy_audio.c toy_audio.h
	$(CC) $(CFLAGS) -c toy_audio.c -o $@

clean:
	$(RM) libtoyaudio.a toy_audio.o

.PHONY: all clean
