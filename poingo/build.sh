#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${ROOT_DIR}/third_party/sysroot"

setup_sysroot_env() {
    [ -d "${SYSROOT}/usr" ] || return 0
    export PKG_CONFIG_PATH="${SYSROOT}/usr/lib64/pkgconfig:${SYSROOT}/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CUBEB_PREFIX="${SYSROOT}/usr"
}
setup_sysroot_env

check_deps() {
    DEPS_MISSING=0

    if ! pkg-config --exists wayland-client wayland-egl egl glesv2 xkbcommon >/dev/null 2>&1; then
        echo "-> Wayland/EGL/GLES/xkbcommon development libraries are missing."
        DEPS_MISSING=1
    fi

    if [ ! -f "${CUBEB_PREFIX:-/usr}/include/cubeb/cubeb.h" ] && ! pkg-config --exists libcubeb >/dev/null 2>&1; then
        echo "-> cubeb development libraries are missing."
        DEPS_MISSING=1
    fi

    if [ ! -f "${ROOT_DIR}/third_party/lodepng/lodepng.h" ] || [ ! -f "${ROOT_DIR}/third_party/lodepng/lodepng.c" ]; then
        echo "-> lodepng source files for icon generation are missing."
        DEPS_MISSING=1
    fi
}

echo "Checking dependencies..."
check_deps

if [ "$DEPS_MISSING" -eq 1 ]; then
    echo "Required dependencies are missing. Automatically invoking getdeps.sh to install them..."
    "${ROOT_DIR}/getdeps.sh"
    setup_sysroot_env
    check_deps
    if [ "$DEPS_MISSING" -eq 1 ]; then
        echo "Error: dependencies are still missing after running getdeps.sh." >&2
        exit 1
    fi
else
    echo "All dependencies (Wayland, EGL/GLES, cubeb) are present."
fi

echo "Building Poingo..."
make clean
make poingo

echo "Build complete! You can run the executable at: ./bin/poingo"
