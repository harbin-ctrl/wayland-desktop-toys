PKG_CONFIG ?= pkg-config

TOY_AUDIO_PIPEWIRE_CFLAGS_RAW := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
TOY_AUDIO_PIPEWIRE_LIBS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)

ifeq ($(strip $(TOY_AUDIO_PIPEWIRE_LIBS)),)
$(error PipeWire development files are required to build the desktop toys)
endif

# PipeWire's public headers intentionally use GNU C extensions. Treat them as
# system headers so a toy's -pedantic policy only diagnoses project code.
TOY_AUDIO_PIPEWIRE_CFLAGS := $(subst -I,-isystem ,$(TOY_AUDIO_PIPEWIRE_CFLAGS_RAW))
