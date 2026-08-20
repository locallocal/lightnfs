#!/usr/bin/env bash
set -euo pipefail

server=${1:-127.0.0.1}
export_path=${2:?usage: accept_m1.sh SERVER EXPORT_PATH [NFS_PORT] [MOUNT_PORT] [MANIFEST]}
nfs_port=${3:-2049}
mount_port=${4:-20048}
manifest=${5:-}

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

mount -t nfs -o "vers=3,tcp,ro,nolock,port=${nfs_port},mountport=${mount_port}" \
  "${server}:${export_path}" "$mount_dir"

ls -lR "$mount_dir" >/dev/null
find "$mount_dir" -type f -print0 | xargs -0 -r cat >/dev/null
if [[ -n "$manifest" ]]; then
  (cd "$mount_dir" && md5sum -c "$manifest")
else
  find "$mount_dir" -type f -print0 | sort -z | xargs -0 -r md5sum >/dev/null
fi

if [[ -n ${CTHON_DIR:-} ]]; then
  "$CTHON_DIR/server" -p "$mount_dir"
fi

echo "M1 read-only mount acceptance passed"
