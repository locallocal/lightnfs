#!/usr/bin/env bash
# Debian package (plan doc 10 §4.5): dpkg-deb only, no debhelper toolchain needed.
# Installs under /usr, config example under /etc/lightnfs. Output in packaging/dist/.
set -euo pipefail
. "$(dirname "$0")/build_common.sh"
command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found" >&2; exit 1; }

build_release
stage /usr
# dpkg convention: configuration under /etc, not /usr/etc.
if [ -d "$STAGE/usr/etc" ]; then
  mkdir -p "$STAGE/etc"
  mv "$STAGE/usr/etc/"* "$STAGE/etc/"
  rmdir "$STAGE/usr/etc"
fi

case "$(uname -m)" in
  x86_64) DEB_ARCH=amd64 ;;
  aarch64) DEB_ARCH=arm64 ;;
  *) DEB_ARCH="$(uname -m)" ;;
esac

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: lightnfs
Version: $VERSION
Section: net
Priority: optional
Architecture: $DEB_ARCH
Maintainer: lightnfs maintainers <lightnfs@example.invalid>
Depends: libc6, libstdc++6
Description: Userspace NFS gateway (NFSv3 + NFSv4.1/4.2)
 io_uring-based userspace NFS server with a local filesystem backend,
 Prometheus metrics and a unix-socket admin interface (lightnfs-ctl).
EOF

DIST="$ROOT/packaging/dist"
mkdir -p "$DIST"
OUT="$DIST/lightnfs_${VERSION}_${DEB_ARCH}.deb"
dpkg-deb --root-owner-group -b "$STAGE" "$OUT" >/dev/null
echo "built $OUT"
