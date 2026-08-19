#!/usr/bin/env bash
# Fetch and build the vendored liburing used when the system liburing-dev is absent.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=liburing-2.14
if [ -f third_party/liburing/src/liburing.a ]; then
  echo "vendored liburing already built"
  exit 0
fi
mkdir -p third_party
curl -sL -o third_party/liburing.tar.gz \
  "https://github.com/axboe/liburing/archive/refs/tags/${VER}.tar.gz"
tar -C third_party -xzf third_party/liburing.tar.gz
rm -rf third_party/liburing third_party/liburing.tar.gz.d
mv "third_party/liburing-${VER}" third_party/liburing
rm third_party/liburing.tar.gz
(cd third_party/liburing && ./configure && make -C src -j"$(nproc)" liburing.a)
echo "vendored liburing built at third_party/liburing/src/liburing.a"
