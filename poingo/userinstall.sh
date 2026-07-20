#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "${ROOT_DIR}/bin/poingo" ]; then
    echo "Poingo is not built yet. Building now..."
    "${ROOT_DIR}/build.sh"
fi

echo "Installing Poingo for the current user (local directory: ~/.local)..."
make install-user

echo "User installation complete!"
