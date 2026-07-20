.DEFAULT_GOAL := all

# Check if required libraries exist on the host/container
HAS_LIBS := $(shell pkg-config --exists wayland-client wayland-egl egl glesv2 2>/dev/null && echo yes || echo no)

.PHONY: distrobox
distrobox:
	@echo "=== Setting up Distrobox Build Environment ==="
	@if ! command -v distrobox >/dev/null 2>&1; then \
		echo "Distrobox not found. Installing distrobox..."; \
		if command -v curl >/dev/null 2>&1; then \
			curl -s https://raw.githubusercontent.com/89luca89/distrobox/main/install | sudo sh; \
		else \
			echo "curl is required to install distrobox. Please install curl and try again."; \
			exit 1; \
		fi; \
	else \
		echo "Distrobox is already installed."; \
	fi
	@if ! command -v podman >/dev/null 2>&1 && ! command -v docker >/dev/null 2>&1; then \
		echo "Neither podman nor docker found. Attempting to install podman..."; \
		if command -v apt-get >/dev/null 2>&1; then \
			sudo apt-get update && sudo apt-get install -y podman; \
		elif command -v dnf >/dev/null 2>&1; then \
			sudo dnf install -y podman; \
		elif command -v pacman >/dev/null 2>&1; then \
			sudo pacman -S --noconfirm podman; \
		else \
			echo "Please install podman or docker manually and try again."; \
			exit 1; \
		fi; \
	fi
	@if ! distrobox list 2>/dev/null | grep -q 'dev'; then \
		echo "Creating 'dev' distrobox container..."; \
		distrobox create --image quay.io/fedora/fedora:40 --name dev --yes; \
	else \
		echo "Distrobox container 'dev' already exists."; \
	fi
	@echo "Installing build dependencies inside the 'dev' container..."
	@distrobox-enter -n dev -- sudo dnf install -y gcc make pkgconf-pkg-config wayland-devel mesa-libEGL-devel mesa-libGLES-devel cubeb-devel
	@echo "Building the software inside the container..."
	@distrobox-enter -n dev -- $(MAKE) --no-print-directory all
	@echo "=== Distrobox Build Environment Setup & Build Complete! ==="

ifeq ($(HAS_LIBS),no)
# Check if we are already inside a container
INSIDE_CONTAINER := $(shell [ -f /run/.containerenv ] && echo yes || echo no)
ifeq ($(INSIDE_CONTAINER),no)
# See if distrobox and the 'dev' container are available
HAS_DISTROBOX := $(shell command -v distrobox >/dev/null 2>&1 && distrobox list 2>/dev/null | grep -q 'dev' && echo yes || echo no)
ifeq ($(HAS_DISTROBOX),yes)
$(info [Host Build] Missing local dev dependencies. Forwarding build to distrobox 'dev' container...)
%:
	@distrobox-enter -n dev -- $(MAKE) --no-print-directory $@
all:
	@distrobox-enter -n dev -- $(MAKE) --no-print-directory all
.PHONY: all
else
$(error Missing Wayland/EGL/GLESv2 development headers. Please install them, or run 'make distrobox' to automatically install and set up a dev container)
endif
else
$(error Missing Wayland/EGL/GLESv2 development headers inside container.)
endif
else

CC     := gcc
CFLAGS := -O3 -ffast-math -Wall -pthread $(shell pkg-config --cflags wayland-client wayland-egl egl glesv2)
LIBS   := $(shell pkg-config --libs wayland-client wayland-egl egl glesv2) -lm -pthread

# cubeb for the audio subsystem (from Poingo): prefer pkg-config, then the
# distro's headers + static lib (Fedora ships no .pc file); without either
# the build still works but runs silent.
CUBEB_PREFIX ?= /usr
CUBEB_CFLAGS := $(shell pkg-config --cflags libcubeb 2>/dev/null)
CUBEB_LIBS   := $(shell pkg-config --libs libcubeb 2>/dev/null)
ifeq ($(strip $(CUBEB_LIBS)),)
ifneq ($(wildcard $(CUBEB_PREFIX)/include/cubeb/cubeb.h),)
CUBEB_CFLAGS := -I$(CUBEB_PREFIX)/include
ifneq ($(wildcard $(CUBEB_PREFIX)/lib64/libcubeb.a),)
CUBEB_LIBS := $(CUBEB_PREFIX)/lib64/libcubeb.a
else
CUBEB_LIBS := -L$(CUBEB_PREFIX)/lib64 -lcubeb
endif
endif
endif
ifneq ($(strip $(CUBEB_LIBS)),)
CFLAGS += -DHAVE_CUBEB $(CUBEB_CFLAGS)
LIBS   += $(CUBEB_LIBS) -lstdc++ -ldl
else
$(warning cubeb not found; building without audio)
endif

TARGET  := balloons
BIN_DIR := bin
RINGMENU_DIR ?= $(CURDIR)/../ring-menu
RINGMENU_LIB := $(RINGMENU_DIR)/libringmenu.a
TOYAUDIO_DIR ?= $(CURDIR)/../toy-audio
DESKTOP_TOYS_DIR ?= $(CURDIR)/../desktop-toys-packaging
TOYAUDIO_LIB := $(TOYAUDIO_DIR)/libtoyaudio.a
CFLAGS  += -I$(RINGMENU_DIR) -I$(TOYAUDIO_DIR) -I$(CURDIR)/../third_party/lodepng
GHOSTICON_DIR := $(CURDIR)/../shared
GHOSTICON_LIB := $(GHOSTICON_DIR)/libghosticon.a

LODEPNG_DIR := $(CURDIR)/../third_party/lodepng
LODEPNG_LIB := $(LODEPNG_DIR)/liblodepng.a
COMMON  := xdg-shell-protocol.o xdg-decoration-protocol.o
OBJS    := balloons.o balloon_gen.o audio.o $(COMMON)

PREFIX := /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: all clean install uninstall install-user uninstall-user icons

all: $(TARGET)

$(TARGET): $(OBJS) $(LODEPNG_LIB) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB)
	$(CC) -o $@ $(OBJS) $(LODEPNG_LIB) $(RINGMENU_LIB) $(TOYAUDIO_LIB) $(GHOSTICON_LIB) $(LIBS)

balloons.o: balloons.c balloon_gen.h audio.h $(RINGMENU_DIR)/ringmenu.h xdg-shell-client-protocol.h xdg-decoration-client-protocol.h $(GHOSTICON_DIR)/ghost_icon.h
	$(CC) $(CFLAGS) -I$(GHOSTICON_DIR) -c -o $@ $<

balloon_gen.o: balloon_gen.c balloon_gen.h
	$(CC) $(CFLAGS) -c -o $@ $<

audio.o: audio.c audio.h $(TOYAUDIO_DIR)/toy_audio.h
	$(CC) $(CFLAGS) -c -o $@ $<

# The ring menu and audio core live in their own library checkouts;
# delegate so they rebuild whenever their sources change.
$(RINGMENU_LIB): FORCE
	$(MAKE) -C $(RINGMENU_DIR)
$(TOYAUDIO_LIB): FORCE
	$(MAKE) -C $(TOYAUDIO_DIR)
$(GHOSTICON_LIB): FORCE
	$(MAKE) -C $(GHOSTICON_DIR)
FORCE:

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-decoration-protocol.o: xdg-decoration-protocol.c xdg-decoration-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-decoration-client-protocol.h:
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

xdg-decoration-protocol.c:
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

clean:
	$(RM) $(TARGET) $(OBJS)
	$(RM) xdg-*.h xdg-*.c $(BIN_DIR)/icon_maker icon_maker.o balloon_assets.h thunder_pcm.h
	$(RM) assets/*.apng assets/icon*.png assets/icon.png
	$(RM) assets/.apngs_generated assets/.pops_generated

ICON_SIZES    := 16 32 48 64 128 256 512

DATADIR := $(PREFIX)/share

include $(DESKTOP_TOYS_DIR)/install.mk

# System install: binary plus the desktop entry, Desktop Toys menu and
# every icon size — the same set the user install ships, like splat.
icon_maker.o: CFLAGS := -O3 -Wall -I$(CURDIR)/../third_party/lodepng
$(BIN_DIR)/icon_maker: icon_maker.o $(LODEPNG_LIB) | $(BIN_DIR)
	$(CC) -o $@ $^ -lm

$(BIN_DIR):
	mkdir -p $@

icons: $(BIN_DIR)/icon_maker
	$(BIN_DIR)/icon_maker assets

install: $(TARGET) icons desktop-toys-install
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -Dm644 balloons.desktop "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		install -Dm644 assets/icon_$${size}x$${size}.png \
			"$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true

uninstall: desktop-toys-uninstall
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(RM) "$(DESTDIR)$(DATADIR)/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	update-desktop-database "$(DESTDIR)$(DATADIR)/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true

install-user: $(TARGET) icons desktop-toys-install-user
	install -d "$(HOME)/.local/bin"
	install -m 755 $(TARGET) "$(HOME)/.local/bin/$(TARGET)"
	install -d "$(HOME)/.local/share/applications"
	install -m 644 balloons.desktop "$(HOME)/.local/share/applications/balloons.desktop"
	install -d "$(HOME)/.local/share/icons/hicolor"
	for size in $(ICON_SIZES); do \
		install -d "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps"; \
		install -m 644 assets/icon_$${size}x$${size}.png "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	install -d "$(HOME)/.local/share/pixmaps"
	install -m 644 assets/icon_256x256.png "$(HOME)/.local/share/pixmaps/balloons.png"
	install -m 644 $(DESKTOP_TOYS_DIR)/desktop-toys-icon-256.png "$(HOME)/.local/share/pixmaps/desktop-toys.png"
	install -d "$(HOME)/.icons"
	install -m 644 assets/icon_256x256.png "$(HOME)/.icons/balloons.png"
	install -m 644 $(DESKTOP_TOYS_DIR)/desktop-toys-icon-256.png "$(HOME)/.icons/desktop-toys.png"
	update-desktop-database "$(HOME)/.local/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(HOME)/.local/share/icons/hicolor" 2>/dev/null || true

uninstall-user: desktop-toys-uninstall-user
	$(RM) "$(HOME)/.local/bin/$(TARGET)"
	$(RM) "$(HOME)/.local/share/applications/balloons.desktop"
	for size in $(ICON_SIZES); do \
		$(RM) "$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/balloons.png"; \
	done
	$(RM) "$(HOME)/.local/share/pixmaps/balloons.png"
	$(RM) "$(HOME)/.local/share/pixmaps/desktop-toys.png"
	$(RM) "$(HOME)/.icons/balloons.png"
	$(RM) "$(HOME)/.icons/desktop-toys.png"
	update-desktop-database "$(HOME)/.local/share/applications" 2>/dev/null || true
	gtk-update-icon-cache -f -t -q "$(HOME)/.local/share/icons/hicolor" 2>/dev/null || true

endif
