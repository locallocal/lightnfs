#!/usr/bin/env bash
# Binary tarball (plan doc 10 §4.5): lightnfs-<version>-linux-<arch>.tar.gz with the
# usual /usr/local layout (bin/, etc/lightnfs/, lib/systemd/system/, share/doc/).
# Output lands in packaging/dist/.
set -euo pipefail
. "$(dirname "$0")/build_common.sh"

build_release
stage /usr/local

DIST="$ROOT/packaging/dist"
mkdir -p "$DIST"
ARCH="$(uname -m)"
OUT="$DIST/lightnfs-$VERSION-linux-$ARCH.tar.gz"
tar -C "$STAGE" -czf "$OUT" .
echo "built $OUT"
