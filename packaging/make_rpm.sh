#!/usr/bin/env bash
# RPM package (plan doc 10 §4.5): stages via CMake install, then packages the staged
# tree with rpmbuild (no source rpm, no network). Output in packaging/dist/.
set -euo pipefail
. "$(dirname "$0")/build_common.sh"
command -v rpmbuild >/dev/null || { echo "rpmbuild not found" >&2; exit 1; }

build_release
stage /usr
# rpm convention: config under /etc; CMake's SYSCONFDIR under a /usr prefix is /usr/etc.
if [ -d "$STAGE/usr/etc" ]; then
  mkdir -p "$STAGE/etc"
  mv "$STAGE/usr/etc/"* "$STAGE/etc/"
  rmdir "$STAGE/usr/etc"
fi

DIST="$ROOT/packaging/dist"
mkdir -p "$DIST"
RPMTOP="$BUILD_DIR/rpmbuild"
rm -rf "$RPMTOP"
mkdir -p "$RPMTOP"/{BUILD,RPMS,SPECS}
rpmbuild -bb "$ROOT/packaging/lightnfs.spec" \
  --define "_topdir $RPMTOP" \
  --define "lnfs_version $VERSION" \
  --define "lnfs_stage $STAGE" \
  --define "_libdir /usr/lib" >/dev/null
find "$RPMTOP/RPMS" -name '*.rpm' -exec cp {} "$DIST/" \;
echo "built $(find "$DIST" -name "lightnfs-$VERSION*.rpm" | tail -1)"
