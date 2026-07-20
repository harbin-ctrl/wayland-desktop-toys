#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"


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

if [ -f /run/ostree-booted ]; then
    echo "Immutable (OSTree-based) system detected: ${OS_ID}"
    for tool in dnf rpm2cpio cpio; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Error: '$tool' not found. Install it, or build inside a container instead:" >&2
            echo "  distrobox create --name poingo-dev --image registry.fedoraproject.org/fedora:latest" >&2
            echo "  distrobox enter poingo-dev -- ./build.sh" >&2
            exit 1
        fi
    done

    SYSROOT="${ROOT_DIR}/third_party/sysroot"
    RPM_DIR="${SYSROOT}/.rpms"
    BASEARCH="$(uname -m)"
    RPM_DEPS=(
        wayland-devel
        libglvnd-devel
        libxkbcommon-devel
        cubeb-devel
    )

    mkdir -p "$RPM_DIR"
    echo "Downloading devel packages into local sysroot (no system changes)..."
    dnf download --resolve --setopt='*.exclude=' --arch "$BASEARCH" --arch noarch \
        --destdir "$RPM_DIR" "${RPM_DEPS[@]}"

    echo "Extracting packages into ${SYSROOT}..."
    for rpm in "$RPM_DIR"/*.rpm; do
        rpm2cpio "$rpm" | (cd "$SYSROOT" && cpio -idmu --quiet)
    done

    find "$SYSROOT/usr/lib64" "$SYSROOT/usr/lib" -maxdepth 1 -type l 2>/dev/null |
    while read -r link; do
        target="$(readlink "$link")"
        case "$target" in /*) continue ;; esac
        if [ ! -e "$(dirname "$link")/$target" ] && [ -e "/usr/lib64/$target" ]; then
            ln -sfn "/usr/lib64/$target" "$link"
        fi
    done

    find "$SYSROOT/usr/lib64/pkgconfig" "$SYSROOT/usr/share/pkgconfig" \
        -name '*.pc' 2>/dev/null |
    while read -r pc; do
        sed -i \
            -e "s|^prefix=/usr$|prefix=${SYSROOT}/usr|" \
            -e "s|^exec_prefix=/usr$|exec_prefix=${SYSROOT}/usr|" \
            -e "s|^libdir=/usr/|libdir=${SYSROOT}/usr/|" \
            -e "s|^includedir=/usr/|includedir=${SYSROOT}/usr/|" \
            "$pc"
    done

    echo "Sysroot ready at ${SYSROOT}. build.sh picks it up automatically."

elif [[ "$OS_ID" =~ ^(debian|ubuntu|mint|pop|pureos|kali) ]] || [[ "$OS_LIKE" =~ (debian|ubuntu) ]]; then
    echo "Distro family: Debian/Ubuntu"
    DEB_DEPS=(
        build-essential
        pkg-config
        libcubeb-dev
        libwayland-dev
        libegl-dev
        libgles2-mesa-dev
        libxkbcommon-dev
    )
    echo "Running package update..."
    $SUDO apt-get update
    echo "Installing packages: ${DEB_DEPS[*]}"
    $SUDO apt-get install -y "${DEB_DEPS[@]}"
    echo "Dependencies installed successfully!"

elif [[ "$OS_ID" =~ ^(fedora|rhel|centos|rocky|alma|amazon) ]] || [[ "$OS_LIKE" =~ (rhel|fedora) ]]; then
    echo "Distro family: Red Hat / Fedora / CentOS"
    RPM_DEPS=(
        gcc
        make
        pkgconfig
        cubeb-devel
        wayland-devel
        mesa-libEGL-devel
        mesa-libGLES-devel
        libxkbcommon-devel
    )
    
    PKG_MGR="dnf"
    if ! command -v dnf >/dev/null 2>&1; then
        if command -v yum >/dev/null 2>&1; then
            PKG_MGR="yum"
        else
            echo "Error: Neither dnf nor yum package manager found." >&2
            exit 1
        fi
    fi

    echo "Installing packages via $PKG_MGR: ${RPM_DEPS[*]}"
    $SUDO $PKG_MGR install -y "${RPM_DEPS[@]}"
    echo "Dependencies installed successfully!"

else
    echo "Unsupported operating system distribution: ${OS_ID}" >&2
    echo "Please manually install cubeb, Wayland, EGL, and GLES development packages." >&2
    exit 1
fi

