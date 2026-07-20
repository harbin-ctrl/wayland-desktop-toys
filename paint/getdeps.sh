#!/usr/bin/env bash

set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "WARNING: Not running as root and 'sudo' not found. Trying to proceed without sudo..." >&2
    fi
fi

if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="${ID:-unknown}"
    OS_LIKE="${ID_LIKE:-""}"
else
    OS_ID="unknown"
    OS_LIKE="unknown"
fi

echo "Detecting OS: ID=${OS_ID}, LIKE=${OS_LIKE}"

case "${OS_ID} ${OS_LIKE}" in
    *debian*|*ubuntu*)
        $SUDO apt-get update
        $SUDO apt-get install -y \
            build-essential pkg-config \
            libwayland-dev wayland-protocols \
            libegl1-mesa-dev libgles2-mesa-dev \
            libpulse-dev
        ;;
    *fedora*|*rhel*|*centos*)
        $SUDO dnf install -y \
            gcc make pkgconf-pkg-config \
            wayland-devel wayland-protocols-devel \
            mesa-libEGL-devel mesa-libGLES-devel \
            pulseaudio-libs-devel
        ;;
    *)
        echo "Unsupported distro. Install these manually:" >&2
        echo "  wayland dev libs, wayland-protocols, EGL + GLESv2 dev libs," >&2
        echo "  libpulse(-simple) dev libs (optional, for the hiss)." >&2
        exit 1
        ;;
esac

echo "Dependencies installed."
