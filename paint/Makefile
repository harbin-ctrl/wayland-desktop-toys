CC := gcc
BIN_DIR := bin
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
DESKTOP_TOYS_DIR ?= $(CURDIR)/../desktop-toys-packaging
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
PAINT_SRC := src/paint.c xdg-shell-protocol.c
ICON_SRC := icon_maker.c
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a
LODEPNG_LIB := $(CURDIR)/../third_party/lodepng/liblodepng.a
TARGETS := $(BIN_DIR)/paint $(BIN_DIR)/icon_maker

ICON_SIZES := 32 48 64 128 192 256 512 1024
ICON_PNGS := $(addprefix icon_,$(addsuffix .png,$(ICON_SIZES)))

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

include $(DESKTOP_TOYS_DIR)/install.mk

# Spray is a pure-Wayland client: Wayland + EGL/GLES for the overlay, and
# (optionally) PulseAudio for the spray-can hiss. No SDL, no toolkit.
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl egl glesv2 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl egl glesv2 2>/dev/null)
ifeq ($(strip $(WAYLAND_LIBS)),)
WAYLAND_LIBS := -lwayland-client -lwayland-egl -lEGL -lGLESv2
endif

# The hiss is optional: without libpulse-simple the toy builds silent.
PULSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpulse-simple 2>/dev/null)
PULSE_LIBS := $(shell $(PKG_CONFIG) --libs libpulse-simple 2>/dev/null)
AUDIO_CFLAGS :=
AUDIO_LIBS :=
ifneq ($(strip $(PULSE_LIBS)),)
AUDIO_CFLAGS := -DHAVE_PULSE $(PULSE_CFLAGS)
AUDIO_LIBS := $(PULSE_LIBS)
else
$(info libpulse-simple not found; building without the spray hiss.)
endif

# The paint rasterizer is hot float loops; build vectorization-friendly but
# portable (no -mcpu=native, so the binary can be packaged for other machines).
CFLAGS := -std=c11 -Wall -Wextra -Wshadow -pedantic -D_GNU_SOURCE -D_DEFAULT_SOURCE \
          -O3 -ffast-math -funroll-loops -fPIC -I$(CURDIR) -I$(RINGMENU_DIR) \
          -I$(TOYAUDIO_DIR) -I$(GHOSTICON_DIR) -I$(CURDIR)/../third_party/lodepng $(WAYLAND_CFLAGS) $(AUDIO_CFLAGS)
LDFLAGS := $(WAYLAND_LIBS) $(AUDIO_LIBS) -lm -lpthread

.PHONY: all clean run icons paint icon_maker install install-user test

all: $(TARGETS)

paint: $(BIN_DIR)/paint

# Wayland protocol glue is generated, not checked in.
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)
WAYLAND_SCANNER := $(shell command -v wayland-scanner 2>/dev/null || echo wayland-scanner)

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

src/paint.o: xdg-shell-client-protocol.h $(RINGMENU_DIR)/ringmenu.h \
		$(TOYAUDIO_DIR)/toy_audio.h $(GHOSTICON_DIR)/ghost_icon.h

# The ring menu and audio core live in their own library checkouts; delegate
# so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

$(BIN_DIR)/paint: $(PAINT_SRC:.c=.o) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

icon_maker: $(BIN_DIR)/icon_maker

icon_maker.o: CFLAGS := -std=c99 -O2 -I$(CURDIR) -I$(CURDIR)/../third_party/lodepng
$(LODEPNG_LIB): FORCE
	$(MAKE) -C $(CURDIR)/../third_party/lodepng
$(BIN_DIR)/icon_maker: icon_maker.o $(LODEPNG_LIB) | $(BIN_DIR)
	$(CC) -O2 $^ -o $@ -lm

icons: icon_maker
	$(BIN_DIR)/icon_maker

run: $(BIN_DIR)/paint
	./$(BIN_DIR)/paint

# Offscreen engine test: includes src/paint.c directly and writes PNGs of a
# simulated session (spray, mixing, drips, eraser, cursor art) plus checks.
test: $(BIN_DIR)/test_engine
	cd $(CURDIR) && ./$(BIN_DIR)/test_engine

$(BIN_DIR)/test_engine: test_engine.c src/paint.c spraycan.h \
		xdg-shell-client-protocol.h $(RINGMENU_LIB) \
		xdg-shell-protocol.o $(LODEPNG_LIB) | $(BIN_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -I$(CURDIR) -I$(RINGMENU_DIR) \
		-I$(TOYAUDIO_DIR) -I$(CURDIR)/../third_party/lodepng test_engine.c \
		$(RINGMENU_LIB) xdg-shell-protocol.o $(LODEPNG_LIB) \
		-o $@ $(WAYLAND_LIBS) -lm -lpthread

install: $(BIN_DIR)/paint icons desktop-toys-install
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BIN_DIR)/paint "$(DESTDIR)$(PREFIX)/bin/paint"
	install -d "$(DESTDIR)$(PREFIX)/share/applications"
	install -m 644 paint.desktop "$(DESTDIR)$(PREFIX)/share/applications/paint.desktop"
	install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/paint.png"; \
	done
	update-desktop-database "$(DESTDIR)$(PREFIX)/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

install-user: $(BIN_DIR)/paint icons desktop-toys-install-user
	install -d "$(HOME)/.local/bin"
	install -m 755 $(BIN_DIR)/paint "$(HOME)/.local/bin/paint"
	install -d "$(HOME)/.local/share/applications"
	install -m 644 paint.desktop "$(HOME)/.local/share/applications/paint.desktop"
	install -d "$(HOME)/.local/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 icon_$${size}.png "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/paint.png"; \
	done
	update-desktop-database "$(HOME)/.local/share/applications" 2>/dev/null || true
	test -f "$(HOME)/.local/share/icons/hicolor/index.theme" || cp /usr/share/icons/hicolor/index.theme "$(HOME)/.local/share/icons/hicolor/" 2>/dev/null || true
	gtk-update-icon-cache -f -q "$(HOME)/.local/share/icons/hicolor" 2>/dev/null || true

$(BIN_DIR):
	mkdir -p $(BIN_DIR)


clean:
	$(RM) $(TARGETS) $(BIN_DIR)/test_engine $(PAINT_SRC:.c=.o) icon_maker.o test_engine.o $(LODEPNG_LIB) $(ICON_PNGS)
	$(RM) xdg-shell-client-protocol.h xdg-shell-protocol.c
	$(RM) test_*.png
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
