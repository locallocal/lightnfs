#!/usr/bin/env bash
# Builds the xfstests fsx binary standalone for the phase-2 acceptance (development
# plan §4.6: fsx over an NFS mount).  Clones xfstests shallowly and replaces its
# generated global.h with a minimal static shim — only fsx is built, nothing else.
#
# usage: fetch_fsx.sh DEST_DIR      # produces DEST_DIR/fsx
set -euo pipefail

dest=${1:?usage: fetch_fsx.sh DEST_DIR}
mkdir -p "$dest"

if [[ ! -x $dest/fsx ]]; then
  src="$dest/xfstests"
  if [[ ! -d $src ]]; then
    git clone --depth 1 https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git "$src"
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
