#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "${ROOT_DIR}/bin/poingo" ]; then
    echo "Poingo is not built yet. Building now..."
    "${ROOT_DIR}/build.sh"
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "WARNING: Not running as root and 'sudo' not found. Trying to proceed without sudo..." >&2
    fi
fi

echo "Installing Poingo system-wide..."
$SUDO make install

if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    sudo chown -R "$(id -u):$(id -g)" "${ROOT_DIR}"
fi

echo "System installation complete!"
