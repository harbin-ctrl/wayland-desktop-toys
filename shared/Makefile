ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
CFLAGS ?= -O3 -Wall -fPIC

all: libghosticon.a

ghost_icon.o: ghost_icon.c ghost_icon.h
	$(CC) $(CFLAGS) -c -o $@ $<

libghosticon.a: ghost_icon.o
	ar rcs $@ $^

clean:
	rm -f *.o *.a
