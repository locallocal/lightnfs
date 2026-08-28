#!/usr/bin/env bash
# Shared staging logic for the packaging scripts (plan doc 10 §4.5).  Builds a
# Release tree and installs it into a DESTDIR staging root via CMake's install
# rules, so every package format ships the same file set.
#
# Usage: source this file, then call `stage <prefix>`; afterwards $STAGE holds the
# staging root and $VERSION the project version from CMakeLists.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${LNFS_PKG_BUILD_DIR:-$ROOT/build-pkg}"
VERSION="$(sed -n 's/^project(lightnfs VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "cannot read version from CMakeLists.txt" >&2; exit 1; }

build_release() {
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" -j"$(nproc)" --target lightnfsd lightnfs-ctl lightnfs-fh
}

# stage <install-prefix>: install into a fresh staging root under $BUILD_DIR.
stage() {
  local prefix="$1"
  STAGE="$BUILD_DIR/stage"
  rm -rf "$STAGE"
  DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix "$prefix" --strip >/dev/null
}
