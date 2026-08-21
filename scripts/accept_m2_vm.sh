#!/usr/bin/env bash
# Phase-2 kernel-mount acceptance on a root VM (development plan §4.6): real
# `mount -o vers=3` read-write, cthon04 basic/general/special suites, fsx, and a
# kill -9 remount-free recovery check.  Reused by CI (the runner is a root VM) and by
# any test VM; overnight fsx is the same script with FSX_OPS raised.
#
# usage: accept_m2_vm.sh [FSX_OPS]     # default 50000 ops (~minutes); overnight: 10000000
set -euo pipefail

fsx_ops=${1:-50000}
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12099}
mount_port=${LNFS_MOUNT_PORT:-12098}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m2vm.XXXXXX")
data="$work/data"
mount_dir="$work/mnt"

server_pid=""
cleanup() {
  if mountpoint -q "$mount_dir" 2>/dev/null; then sudo umount -f "$mount_dir" || true; fi
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building lightnfsd (Release)"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd >/dev/null

echo "== fetching test suites"
"$repo/scripts/fetch_cthon.sh" "$work/cthon04"
make -C "$work/cthon04" 2>/dev/null >/dev/null || make -C "$work/cthon04" >/dev/null
"$repo/scripts/fetch_fsx.sh" "$work/fsxdir"

mkdir -p "$data" "$work/state" "$mount_dir"
cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$work/state"

[protocol]
drc_ttl = "120s"
drc_mem = "64MiB"

[[export]]
path = "$data"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.local]
handles = "auto"
EOF

start_server() {
  "$repo/build-rel/lightnfsd" --config "$work/lightnfs.toml" >>"$work/server.log" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && return 0
    sleep 0.1
  done
  tail -20 "$work/server.log" >&2
  return 1
}

echo "== starting lightnfsd + mount -o vers=3 (rw)"
start_server
sudo mount -t nfs \
  -o "vers=3,tcp,nolock,port=${nfs_port},mountport=${mount_port},timeo=20,retrans=6" \
  "127.0.0.1:${data}" "$mount_dir"
sudo chmod 777 "$mount_dir"

echo "== cthon04 basic/general/special"
export NFSTESTDIR="$mount_dir/cthon-test"
for suite in -b -g -s; do
  echo "---- cthon $suite"
  rm -rf "$NFSTESTDIR"
  (cd "$work/cthon04" && ./runtests "$suite" -f "$NFSTESTDIR")
done

echo "== fsx ($fsx_ops ops)"
"$work/fsxdir/fsx" -N "$fsx_ops" -p 10000 "$mount_dir/fsx.dat"
rm -f "$mount_dir/fsx.dat"

echo "== kill -9 + restart: client-transparent recovery"
cp /etc/hostname "$mount_dir/before-crash" 2>/dev/null || echo probe > "$mount_dir/before-crash"
kill -9 "$server_pid"
server_pid=""
sleep 0.5
start_server
# The same mount must keep working (client retransmits; handles survive restart).
ls "$mount_dir" >/dev/null
echo after > "$mount_dir/after-crash"
cmp "$mount_dir/before-crash" "$data/before-crash"
grep -q after "$data/after-crash"
rm -f "$mount_dir/before-crash" "$mount_dir/after-crash"

sudo umount "$mount_dir"
kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero" >&2; exit 1; }
server_pid=""
echo
echo "M2 VM mount acceptance PASSED (cthon b/g/s + fsx $fsx_ops ops + crash recovery)"
