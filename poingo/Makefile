CC ?= gcc
TARGETS := poingo icon_maker
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
DESKTOP_TOYS_DIR ?= $(CURDIR)/../desktop-toys-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
POINGO_SRC := poingo.c xdg-shell-protocol.c xdg-decoration-unstable-v1-protocol.c
ICON_SRC := icon_maker.c
ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_BMPS := $(addprefix icon_,$(addsuffix .bmp,$(ICON_SIZES)))
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

include $(DESKTOP_TOYS_DIR)/install.mk
include $(TOYAUDIO_DIR)/pipewire.mk

# Poingo uses Wayland, EGL/GLES, and xkbcommon.
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
ifeq ($(strip $(WAYLAND_LIBS)),)
WAYLAND_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lxkbcommon
endif

CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE -O2 -fPIC \
          -I$(CURDIR) -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) -I$(CURDIR)/../third_party/lodepng $(WAYLAND_CFLAGS) $(TOY_AUDIO_PIPEWIRE_CFLAGS)
LDFLAGS := $(WAYLAND_LIBS) $(TOY_AUDIO_PIPEWIRE_LIBS) -lm -lpthread

.PHONY: all clean run debug icons install uninstall install-user uninstall-user

all: $(TARGETS)

# lodepng decodes the Pi logo for the cursor's etched blade (and still
# serves icon_maker as before).
poingo: $(POINGO_SRC:.c=.o) $(LODEPNG_LIB) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# The ring menu and audio core live in their own library checkouts;
# delegate so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
icon_maker: icon_maker.o $(LODEPNG_LIB)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker
	./icon_maker

debug: CFLAGS += -O0 -g
debug: all

run: poingo
	./poingo

install: poingo icon_maker icons desktop-toys-install
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 poingo "$(DESTDIR)$(PREFIX)/bin/poingo"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 poingo.desktop "$(DESTDIR)$(PREFIX)/share/applications/poingo.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

install-user: poingo icon_maker icons desktop-toys-install-user
	install -d "$(HOME)/.local/bin"
	install -m 755 poingo "$(HOME)/.local/bin/poingo"
	install -d "$(HOME)/.local/share/applications"
	install -m 644 poingo.desktop "$(HOME)/.local/share/applications/poingo.desktop"
	install -d "$(HOME)/.local/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	update-desktop-database "$(HOME)/.local/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(HOME)/.local/share/icons/hicolor" 2>/dev/null || true

uninstall: desktop-toys-uninstall
	$(RM) "$(DESTDIR)$(PREFIX)/bin/poingo"
	$(RM) "$(DESTDIR)$(PREFIX)/share/applications/poingo.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

uninstall-user: desktop-toys-uninstall-user
	$(RM) "$(HOME)/.local/bin/poingo"
	$(RM) "$(HOME)/.local/share/applications/poingo.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	update-desktop-database "$(HOME)/.local/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(HOME)/.local/share/icons/hicolor" 2>/dev/null || true

WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-decoration-unstable-v1-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

xdg-decoration-unstable-v1-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

poingo.o: xdg-shell-client-protocol.h xdg-decoration-unstable-v1-client-protocol.h \
	$(TOYAUDIO_DIR)/toy_audio.h $(TOYAUDIO_DIR)/toy_audio_stream.h

clean:
	$(RM) $(TARGETS) $(POINGO_SRC:.c=.o) $(ICON_SRC:.c=.o) $(LODEPNG_LIB) $(ICON_BMPS) $(ICON_PNGS)
	$(RM) poingo_*.png xdg-*.h xdg-*.c
	$(RM) -rf third_party/sysroot

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
