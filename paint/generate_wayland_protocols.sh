#!/bin/bash

set -e

PROTOCOLS_DIR="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || echo /usr/share/wayland-protocols)"

echo "Generating Wayland protocol files..."
echo "  - xdg-shell..."
wayland-scanner client-header \
    "$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml" \
    xdg-shell-client-protocol.h

wayland-scanner private-code \
    "$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml" \
    xdg-shell-protocol.c

echo "Done! Protocol files generated successfully."
