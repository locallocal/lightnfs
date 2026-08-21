#!/usr/bin/env bash
# Runs the read-only part of the cthon04 basic suite against an already-mounted
# NFS directory (development plan §3.5, milestone M1).
#
# The M1 server is read-only, so only the basic tests that never write can run:
#   test3   lookups across the mount point (chdir/getcwd/stat)
#   test5b  read (bigfile pre-staged by gen_dataset.sh; CTHON_RO=1 skips the
#           trailing unlink cleanup — see fetch_cthon.sh)
#   test9   statvfs
# test1/2/4/5/5a/6/7/8 create, write, rename or remove and belong to the phase-2
# (read-write) acceptance in §4.6.
#
# usage: cthon_ro.sh CTHON_DIR MOUNTED_TESTDIR
set -euo pipefail

cthon=${1:?usage: cthon_ro.sh CTHON_DIR MOUNTED_TESTDIR}
testdir=${2:?usage: cthon_ro.sh CTHON_DIR MOUNTED_TESTDIR}

[[ -x $cthon/basic/test3 ]] || { echo "cthon not built; run fetch_cthon.sh" >&2; exit 1; }
[[ -f $testdir/bigfile ]] || { echo "$testdir/bigfile missing (gen_dataset.sh stages it)" >&2; exit 1; }

export NFSTESTDIR=$testdir
export CTHON_RO=1
size=$(stat -c %s "$testdir/bigfile")

echo "== cthon04 basic read-only subset (NFSTESTDIR=$NFSTESTDIR)"
"$cthon/basic/test3" -n 250
"$cthon/basic/test5b" -n "$size" 10
"$cthon/basic/test9" -n 1500
echo "== cthon04 basic read-only subset PASSED"
