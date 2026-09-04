#!/usr/bin/env bash
# Checks backend/cephfs/cephapi.hpp against the installed libcephfs headers (design 06 §6.8,
# plan doc 10 §5.3): every function-pointer member of cephapi::Api must have exactly
# the type of the corresponding libcephfs declaration, and the two structs the
# backend defines itself (ceph_statx, vinodeno_t) must have the real layout.  The
# backend loads the library at runtime, so this is the only place a signature drift
# would be caught at build time.
#
#   scripts/check_cephapi_abi.sh [include-dir-with-cephfs/]
#
# Exit 0: all signatures match, or headers not found (skipped; LNFS_CEPHAPI_STRICT=1
# makes that exit 2 instead).  Exit 1: drift.
set -euo pipefail
cd "$(dirname "$0")/.."
INC="${1:-/usr/include}"
if [ ! -f "$INC/cephfs/libcephfs.h" ]; then
  echo "check_cephapi_abi: no cephfs/libcephfs.h under $INC (install libcephfs-dev); skipped"
  [ "${LNFS_CEPHAPI_STRICT:-0}" = 1 ] && exit 2
  exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Every member name listed in the Api struct becomes one static check.
MEMBERS=$(grep -oE '\(\*(ceph_[a-z_0-9]+)\)' src/backend/cephfs/cephapi.hpp | tr -d '(*)' | sort -u)
{
  # The real header first: its include guard suppresses cephapi.hpp's own
  # ceph_statx / vinodeno_t definitions, so the member types below are compared
  # against the genuine declarations (vinodeno_t stays incomplete under C++, which
  # is fine for a by-value parameter in a function *type*).
  echo '#include <cephfs/libcephfs.h>'
  echo '#include "backend/cephfs/cephapi.hpp"'
  echo '#include <type_traits>'
  echo 'using lnfs::backend::cephapi::Api;'
  # Initialising a member-typed pointer from the real function is the strict check:
  # any signature difference fails to compile.
  for m in $MEMBERS; do
    echo "constexpr decltype(Api::$m) check_$m = &::$m;"
  done
  echo 'namespace c = lnfs::backend::cephapi;'
  echo 'static_assert(CEPH_STATX_MODE == c::kStatxMode && CEPH_STATX_NLINK == c::kStatxNlink);'
  echo 'static_assert(CEPH_STATX_UID == c::kStatxUid && CEPH_STATX_GID == c::kStatxGid);'
  echo 'static_assert(CEPH_STATX_RDEV == c::kStatxRdev && CEPH_STATX_ATIME == c::kStatxAtime);'
  echo 'static_assert(CEPH_STATX_MTIME == c::kStatxMtime && CEPH_STATX_CTIME == c::kStatxCtime);'
  echo 'static_assert(CEPH_STATX_INO == c::kStatxIno && CEPH_STATX_SIZE == c::kStatxSize);'
  echo 'static_assert(CEPH_STATX_BLOCKS == c::kStatxBlocks && CEPH_STATX_BTIME == c::kStatxBtime);'
  echo 'static_assert(CEPH_STATX_VERSION == c::kStatxVersion);'
  echo 'static_assert(CEPH_STATX_BASIC_STATS == c::kStatxBasicStats && CEPH_STATX_ALL_STATS == c::kStatxAllStats);'
  echo 'static_assert(CEPH_SETATTR_MODE == c::kSetMode && CEPH_SETATTR_UID == c::kSetUid);'
  echo 'static_assert(CEPH_SETATTR_GID == c::kSetGid && CEPH_SETATTR_MTIME == c::kSetMtime);'
  echo 'static_assert(CEPH_SETATTR_ATIME == c::kSetAtime && CEPH_SETATTR_SIZE == c::kSetSize);'
  echo 'static_assert(CEPH_SETATTR_CTIME == c::kSetCtime);'
  echo 'static_assert(CEPH_SETATTR_MTIME_NOW == c::kSetMtimeNow && CEPH_SETATTR_ATIME_NOW == c::kSetAtimeNow);'
  echo 'static_assert(AT_STATX_DONT_SYNC == c::kStatxDontSync);'
  echo 'static_assert(FALLOC_FL_KEEP_SIZE == c::kFallocKeepSize && FALLOC_FL_PUNCH_HOLE == c::kFallocPunchHole);'
  echo 'int main() { return 0; }'
} > "$TMP/check.cpp"
# vinodeno_t is only complete in C; its layout (two uint64 by value) and the
# ceph_statx layout are compared there against cephapi.hpp's copies.
{
  echo '#include <stddef.h>'
  echo '#include <cephfs/libcephfs.h>'
  echo '#define CHECK(c) _Static_assert((c), #c)'
  echo 'CHECK(sizeof(vinodeno_t) == 16);'
  echo 'CHECK(offsetof(vinodeno_t, ino) == 0 && offsetof(vinodeno_t, snapid) == 8);'
  echo 'CHECK(sizeof(inodeno_t) == 8 && sizeof(snapid_t) == 8);'
  echo 'CHECK(CEPH_NOSNAP == (uint64_t)-2);  /* cephapi::kNoSnap; C-only macro */'
  echo 'int (*p)(struct ceph_mount_info *, vinodeno_t, Inode **) = ceph_ll_lookup_vino;'
  echo 'struct lnfs_statx {'
  sed -n '/^struct ceph_statx {/,/^};/p' src/backend/cephfs/cephapi.hpp | sed '1d;$d' | sed 's|//.*||'
  echo '};'
  for f in stx_mask stx_blksize stx_nlink stx_uid stx_gid stx_mode stx_ino stx_size stx_blocks \
           stx_dev stx_rdev stx_atime stx_ctime stx_mtime stx_btime stx_version; do
    echo "CHECK(offsetof(struct lnfs_statx, $f) == offsetof(struct ceph_statx, $f));"
  done
  echo 'CHECK(sizeof(struct lnfs_statx) == sizeof(struct ceph_statx));'
  echo 'int main(void) { (void)p; return 0; }'
} > "$TMP/check.c"
CXX="${CXX:-c++}"
CC="${CC:-cc}"
# libcephfs.h insists on _FILE_OFFSET_BITS=64 (a no-op for the ABI on 64-bit hosts,
# where off_t/dirent are already the 64-bit forms the backend is built against).
if "$CXX" -std=c++20 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I src -I "$INC" -fsyntax-only "$TMP/check.cpp" &&
   "$CC" -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I src -I "$INC" -fsyntax-only "$TMP/check.c"; then
  echo "check_cephapi_abi: $(echo "$MEMBERS" | wc -l) libcephfs signatures + struct layouts match ($INC)"
else
  echo "check_cephapi_abi: signature/layout drift against $INC" >&2
  exit 1
fi
