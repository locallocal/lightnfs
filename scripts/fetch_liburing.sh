#!/usr/bin/env bash
# Build the vendored liburing (git submodule at third_party/liburing, pinned tag) for
# use when the system liburing-dev is absent. CMake prefers the submodule only once
# src/liburing.a exists (configure generates compat.h; unbuilt headers are unusable).
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -f third_party/liburing/src/liburing.a ]; then
  echo "vendored liburing already built"
  exit 0
fi
git submodule update --init third_party/liburing
(cd third_party/liburing && ./configure && make -C src -j"$(nproc)" liburing.a)
echo "vendored liburing built at third_party/liburing/src/liburing.a"
