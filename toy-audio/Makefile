CC ?= gcc
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -fPIC

include pipewire.mk

CFLAGS += $(TOY_AUDIO_PIPEWIRE_CFLAGS)

all: libtoyaudio.a

libtoyaudio.a: toy_audio.o toy_audio_stream.o
	$(AR) rcs $@ $^

toy_audio.o: toy_audio.c toy_audio.h
	$(CC) $(CFLAGS) -c toy_audio.c -o $@

toy_audio_stream.o: toy_audio_stream.c toy_audio_stream.h
	$(CC) $(CFLAGS) -c toy_audio_stream.c -o $@

test: test_mixer
	./test_mixer

test_mixer: test_mixer.c toy_audio.c toy_audio.h
	$(CC) $(CFLAGS) test_mixer.c toy_audio.c -o $@ -lm -pthread

clean:
	$(RM) libtoyaudio.a toy_audio.o toy_audio_stream.o test_mixer

.PHONY: all clean test
