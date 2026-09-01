#!/usr/bin/env bash
# Builds the xfstests fsx binary standalone for the phase-2 acceptance (development
# plan §4.6: fsx over an NFS mount).  Clones xfstests shallowly and replaces its
# generated global.h with a minimal static shim — only fsx is built, nothing else.
#
# usage: fetch_fsx.sh DEST_DIR      # produces DEST_DIR/fsx
#
# Pinned to a fixed xfstests commit (plan doc 10 §7.3); LNFS_FETCH_HEAD=1 takes the
# current upstream HEAD instead (for bumping the pin).
set -euo pipefail

dest=${1:?usage: fetch_fsx.sh DEST_DIR}
mkdir -p "$dest"

pin=56c410ad0f69da5b13c5807bc47b4876dcfa02b2  # upstream HEAD, 2026-09-01

if [[ ! -x $dest/fsx ]]; then
  src="$dest/xfstests"
  if [[ ! -d $src ]]; then
    if [[ ${LNFS_FETCH_HEAD:-0} == 1 ]]; then
      git clone --depth 1 https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git "$src"
    else
      git init -q "$src"
      git -C "$src" fetch -q --depth 1 \
        https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git "$pin"
      git -C "$src" checkout -q FETCH_HEAD
    fi
  fi
  cat > "$src/ltp/global.h" <<'EOF'
/* Minimal standalone shim replacing xfstests' generated global.h (fsx-only build). */
#ifndef GLOBAL_H
#define GLOBAL_H
#define _GNU_SOURCE 1
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>
#define HAVE_LINUX_FALLOC_H 1
#define FALLOCATE 1
#include <linux/falloc.h>
#define roundup_64(x, y) ((((unsigned long long)(x)) + ((y) - 1)) / (y) * (y))
#define rounddown_64(x, y) (((unsigned long long)(x)) / (y) * (y))
#endif
EOF
  gcc -O2 -I"$src/src" -I"$src/ltp" -o "$dest/fsx" "$src/ltp/fsx.c"
fi

echo "fsx ready at $dest/fsx"
