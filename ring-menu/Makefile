ifeq ($(origin CC),default)
CC := ccache gcc
endif
CCACHE_PREFIX ?= distcc
export CC CCACHE_PREFIX
AR ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pedantic -fPIC

all: libringmenu.a

libringmenu.a: ringmenu.o
	$(AR) rcs $@ $^

ringmenu.o: ringmenu.c ringmenu.h
	$(CC) $(CFLAGS) -c ringmenu.c -o $@

clean:
	$(RM) libringmenu.a ringmenu.o

.PHONY: all clean
