#!/usr/bin/env bash
# M1 kernel-mount acceptance (development plan §3.5) — the in-mount half.
# Requires root and an NFS client (run inside a VM or privileged container; the
# one-click wrapper is scripts/accept_m1_container.sh).
#
# Expects the export to be a dataset produced by scripts/gen_dataset.sh
# (tree/ + manifest.md5 + bigdir/ + cthon/bigfile). Verifies:
#   - mount -o vers=3 succeeds
#   - ls -lR, cat of every file, md5sum -c manifest.md5
#   - bigdir: BIGDIR_COUNT entries, no duplicates
#   - write attempts fail (read-only server)
#   - cthon04 basic read-only subset (test3/test5b/test9) when CTHON_DIR is set
#
# usage: accept_m1.sh SERVER EXPORT_PATH [NFS_PORT] [MOUNT_PORT] [BIGDIR_COUNT]
set -euo pipefail

server=${1:-127.0.0.1}
export_path=${2:?usage: accept_m1.sh SERVER EXPORT_PATH [NFS_PORT] [MOUNT_PORT] [BIGDIR_COUNT]}
nfs_port=${3:-2049}
mount_port=${4:-20048}
bigdir_count=${5:-100000}
scripts_dir=$(cd "$(dirname "$0")" && pwd)

if [[ $(id -u) -ne 0 ]]; then
  echo "accept_m1.sh must run as root (mount permission required)" >&2
  exit 2
fi

mount_dir=$(mktemp -d /tmp/lightnfs-m1-mount.XXXXXX)
cleanup() {
  if mountpoint -q "$mount_dir"; then umount "$mount_dir"; fi
  rmdir "$mount_dir"
}
trap cleanup EXIT

echo "== mount -o vers=3 ${server}:${export_path}"
mount -t nfs -o "vers=3,tcp,ro,nolock,port=${nfs_port},mountport=${mount_port}" \
  "${server}:${export_path}" "$mount_dir"

echo "== ls -lR"
ls -lR "$mount_dir" >/dev/null

echo "== cat every file under tree/"
find "$mount_dir/tree" -type f -print0 | xargs -0 -r cat >/dev/null

echo "== md5sum -c manifest.md5"
(cd "$mount_dir" && md5sum --quiet -c manifest.md5)

echo "== bigdir: expect $bigdir_count unique entries"
count=$(ls "$mount_dir/bigdir" | wc -l)
uniq_count=$(ls "$mount_dir/bigdir" | sort -u | wc -l)
if [[ $count -ne $bigdir_count || $uniq_count -ne $bigdir_count ]]; then
  echo "bigdir mismatch: listed=$count unique=$uniq_count expected=$bigdir_count" >&2
  exit 1
fi

echo "== read-only enforcement"
if touch "$mount_dir/should_fail" 2>/dev/null; then
  echo "write on read-only export unexpectedly succeeded" >&2
  exit 1
fi

if [[ -n ${CTHON_DIR:-} ]]; then
  "$scripts_dir/cthon_ro.sh" "$CTHON_DIR" "$mount_dir/cthon"
else
  echo "== CTHON_DIR not set: skipping cthon read-only subset"
fi

echo "M1 read-only mount acceptance passed"
