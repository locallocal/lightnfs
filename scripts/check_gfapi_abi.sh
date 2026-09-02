#!/usr/bin/env bash
# Checks backend/gfapi.hpp against the installed GlusterFS headers (plan doc 10
# §5.3): every function-pointer member of gfapi::Api must have exactly the type of
# the corresponding libgfapi declaration.  The backend loads the library at runtime,
# so this is the only place a signature drift would be caught at build time.
#
#   scripts/check_gfapi_abi.sh [include-dir-with-glusterfs/api]
#
# Exit 0: all signatures match, or headers not found (skipped; LNFS_GFAPI_STRICT=1 makes
# that exit 2 instead).  Exit 1: drift.
set -euo pipefail
cd "$(dirname "$0")/.."
INC="${1:-/usr/include}"
if [ ! -f "$INC/glusterfs/api/glfs-handles.h" ]; then
  echo "check_gfapi_abi: no glusterfs/api headers under $INC (install libglusterfs-dev); skipped"
  [ "${LNFS_GFAPI_STRICT:-0}" = 1 ] && exit 2
  exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Every member name listed in the Api struct becomes one static_assert.
MEMBERS=$(grep -oE '\(\*(glfs_[a-z_]+)\)' src/backend/gfapi.hpp | tr -d '(*)' | sort -u)
{
  echo '#include <glusterfs/api/glfs.h>'
  echo '#include <glusterfs/api/glfs-handles.h>'
  echo '#include "backend/gfapi.hpp"'
  echo '#include <type_traits>'
  echo 'using lnfs::backend::gfapi::Api;'
  # Initialising a member-typed pointer from the real function is the strict check:
  # the only implicit conversion between function pointer types drops noexcept
  # (glfs.h marks everything __THROW), any other difference fails to compile.
  for m in $MEMBERS; do
    echo "constexpr decltype(Api::$m) check_$m = &::$m;"
  done
  echo 'static_assert(GFAPI_HANDLE_LENGTH == lnfs::backend::gfapi::kHandleLength);'
  echo 'static_assert(GFAPI_SET_ATTR_MODE == lnfs::backend::gfapi::kSetMode);'
  echo 'static_assert(GFAPI_SET_ATTR_UID == lnfs::backend::gfapi::kSetUid);'
  echo 'static_assert(GFAPI_SET_ATTR_GID == lnfs::backend::gfapi::kSetGid);'
  echo 'static_assert(GFAPI_SET_ATTR_SIZE == lnfs::backend::gfapi::kSetSize);'
  echo 'static_assert(GFAPI_SET_ATTR_ATIME == lnfs::backend::gfapi::kSetAtime);'
  echo 'static_assert(GFAPI_SET_ATTR_MTIME == lnfs::backend::gfapi::kSetMtime);'
  echo 'static_assert(GFAPI_XREADDIRP_STAT == lnfs::backend::gfapi::kXreaddirpStat);'
  echo 'static_assert(GFAPI_XREADDIRP_HANDLE == lnfs::backend::gfapi::kXreaddirpHandle);'
  echo 'int main() { return 0; }'
} > "$TMP/check.cpp"
CXX="${CXX:-c++}"
if "$CXX" -std=c++20 -D_GNU_SOURCE -I src -I "$INC" -fsyntax-only "$TMP/check.cpp"; then
  echo "check_gfapi_abi: $(echo "$MEMBERS" | wc -l) libgfapi signatures match ($INC)"
else
  echo "check_gfapi_abi: signature drift against $INC" >&2
  exit 1
fi
