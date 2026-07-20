CC ?= gcc
BIN_DIR := bin
TARGETS := $(BIN_DIR)/poingo $(BIN_DIR)/icon_maker
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
DESKTOP_TOYS_DIR ?= $(CURDIR)/../desktop-toys-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
POINGO_SRC := src/poingo.c xdg-shell-protocol.c xdg-decoration-unstable-v1-protocol.c
ICON_SRC := icon_maker.c
ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_BMPS := $(addprefix icon_,$(addsuffix .bmp,$(ICON_SIZES)))
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

include $(DESKTOP_TOYS_DIR)/install.mk

# Poingo uses Wayland, EGL/GLES, and xkbcommon.
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 xkbcommon 2>/dev/null)
ifeq ($(strip $(WAYLAND_LIBS)),)
WAYLAND_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lxkbcommon
endif

# PipeWire drives Poingo's audio device; the shared mixer/DSP lives in
# libtoyaudio.
PIPEWIRE_CFLAGS_RAW := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PIPEWIRE_LIBS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)
ifeq ($(strip $(PIPEWIRE_LIBS)),)
$(error PipeWire development files are required to build poingo)
endif
AUDIO_CFLAGS := $(subst -I,-isystem ,$(PIPEWIRE_CFLAGS_RAW))
AUDIO_LIBS := $(PIPEWIRE_LIBS)

CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE -O2 -fPIC \
          -I$(CURDIR) -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) -I$(CURDIR)/../third_party/lodepng $(WAYLAND_CFLAGS) $(AUDIO_CFLAGS)
LDFLAGS := $(WAYLAND_LIBS) $(AUDIO_LIBS) -lm -lpthread

.PHONY: all clean run debug icons poingo icon_maker install uninstall install-user uninstall-user

all: $(TARGETS)

poingo: $(BIN_DIR)/poingo

# lodepng decodes the Pi logo for the cursor's etched blade (and still
# serves icon_maker as before).
$(BIN_DIR)/poingo: $(POINGO_SRC:.c=.o) $(LODEPNG_LIB) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) | $(BIN_DIR)
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

icon_maker: $(BIN_DIR)/icon_maker

icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
$(BIN_DIR)/icon_maker: icon_maker.o $(LODEPNG_LIB) | $(BIN_DIR)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker
	$(BIN_DIR)/icon_maker

debug: CFLAGS += -O0 -g
debug: all

run: $(BIN_DIR)/poingo
	./$(BIN_DIR)/poingo

install: $(BIN_DIR)/poingo $(BIN_DIR)/icon_maker icons desktop-toys-install
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BIN_DIR)/poingo "$(DESTDIR)$(PREFIX)/bin/poingo"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 poingo.desktop "$(DESTDIR)$(PREFIX)/share/applications/poingo.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/poingo.png"; \
	done
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

install-user: $(BIN_DIR)/poingo $(BIN_DIR)/icon_maker icons desktop-toys-install-user
	install -d "$(HOME)/.local/bin"
	install -m 755 $(BIN_DIR)/poingo "$(HOME)/.local/bin/poingo"
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

src/poingo.o: xdg-shell-client-protocol.h xdg-decoration-unstable-v1-client-protocol.h \
	$(TOYAUDIO_DIR)/toy_audio.h

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	$(RM) $(TARGETS) $(POINGO_SRC:.c=.o) $(ICON_SRC:.c=.o) $(LODEPNG_LIB) $(ICON_BMPS) $(ICON_PNGS)
	$(RM) poingo_*.png xdg-*.h xdg-*.c
	$(RM) -rf third_party/sysroot

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
