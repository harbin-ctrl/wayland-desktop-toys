#!/bin/bash

set -e

echo "Generating Wayland protocol files..."

PROTOCOLS_DIR="/usr/share/wayland-protocols"

echo "  - xdg-shell..."
wayland-scanner client-header \
    "$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml" \
    xdg-shell-client-protocol.h

wayland-scanner private-code \
    "$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml" \
    xdg-shell-protocol.c

echo "  - xdg-decoration..."
wayland-scanner client-header \
    "$PROTOCOLS_DIR/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" \
    xdg-decoration-unstable-v1-client-protocol.h

wayland-scanner private-code \
    "$PROTOCOLS_DIR/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" \
    xdg-decoration-unstable-v1-protocol.c

echo "Done! Protocol files generated successfully."
echo ""
echo "Generated files:"
ls -lh xdg-*.h xdg-*.c
