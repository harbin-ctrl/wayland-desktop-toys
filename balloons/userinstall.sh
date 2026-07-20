#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "${ROOT_DIR}/balloons" ]; then
    echo "Balloons is not built yet. Building now..."
    make -C "${ROOT_DIR}" all
fi

echo "Installing Balloons for the current user (local directory: ~/.local)..."
make -C "${ROOT_DIR}" install-user

echo "User installation complete!"
