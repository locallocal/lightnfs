#!/usr/bin/env bash
# Fetches and builds the Connectathon 2004 test suite (cthon04) for the M1 read-only
# acceptance (development plan §3.5). Applies one patch: with CTHON_RO=1 in the
# environment, basic/test5b skips its final unlink (a cleanup write) so the read test
# can run against a read-only mount. Everything else is upstream-unmodified.
#
# usage: fetch_cthon.sh DEST_DIR
#
# Pinned to a fixed upstream commit (plan doc 10 §7.3) so runs are reproducible and the
# sed patch below cannot silently miss a moved target.  LNFS_FETCH_HEAD=1 fetches the
# current upstream HEAD instead (for bumping the pin).
set -euo pipefail

dest=${1:?usage: fetch_cthon.sh DEST_DIR}

pin=86a4501a6e1e415844dc632894a85a5253cc1505  # upstream HEAD, 2026-09-01
urls=("https://github.com/linux-nfs/cthon04.git"
      "git://git.linux-nfs.org/projects/steved/cthon04.git"
      "https://git.linux-nfs.org/projects/steved/cthon04.git")

if [[ ! -d $dest/.git ]]; then
  cloned=""
  for url in "${urls[@]}"; do
    if [[ ${LNFS_FETCH_HEAD:-0} == 1 ]]; then
      git clone --depth 1 "$url" "$dest" 2>/dev/null && { cloned=$url; break; }
    else
      if git init -q "$dest" &&
        git -C "$dest" fetch -q --depth 1 "$url" "$pin" 2>/dev/null &&
        git -C "$dest" checkout -q FETCH_HEAD; then
        cloned=$url
        break
      fi
      rm -rf "$dest/.git"  # failed attempt: drop only the git metadata
    fi
  done
  [[ -n $cloned ]] || { echo "cannot clone cthon04 @$pin" >&2; exit 1; }
fi

# Read-only mode: make the trailing unlink of test5b conditional.
if ! grep -q CTHON_RO "$dest/basic/test5b.c"; then
  sed -i 's/^\tif (unlink(bigfile) < 0) {/\tif (!getenv("CTHON_RO"))\n\tif (unlink(bigfile) < 0) {/' \
    "$dest/basic/test5b.c"
  grep -q CTHON_RO "$dest/basic/test5b.c" || { echo "test5b patch failed" >&2; exit 1; }
fi

make -C "$dest/basic" test3 test5b test9 >/dev/null 2>&1 || \
  make -C "$dest/basic" test3 test5b test9
echo "cthon04 ready at $dest (basic read-only subset: test3 test5b test9)"
