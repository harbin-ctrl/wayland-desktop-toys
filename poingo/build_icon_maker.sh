#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR="${ROOT_DIR}/bin"
OUT_BIN="${OUT_DIR}/icon_maker"

mkdir -p "${OUT_DIR}"

cc -O2 -std=c99 \
  -I"${ROOT_DIR}/third_party/lodepng" \
  "${ROOT_DIR}/icon_maker.c" \
  "${ROOT_DIR}/third_party/lodepng/lodepng.c" \
  -lm \
  -o "${OUT_BIN}"

printf '%s\n' "${OUT_BIN}"
