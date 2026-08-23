#!/usr/bin/env bash
# One-click M1 mount acceptance on a host/VM with root (sudo) and a kernel NFS client
# — e.g. any root test VM (development plan §3.5).
#
# Builds lightnfsd (Release), generates the dataset, fetches cthon04, starts the
# server on loopback, then runs scripts/accept_m1.sh (real mount -o vers=3, ls -lR /
# cat / md5sum -c, bigdir count, read-only enforcement, cthon basic read-only subset)
# as root.
#
# usage: accept_m1_vm.sh [BIGDIR_COUNT]
set -euo pipefail

bigdir_count=${1:-100000}
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12049}
mount_port=${LNFS_MOUNT_PORT:-12048}
work=$(mktemp -d "${TMPDIR:-/tmp}/lightnfs-m1vm.XXXXXX")
dataset="$work/dataset"

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building lightnfsd (Release)"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd >/dev/null

echo "== generating dataset ($bigdir_count bigdir entries)"
"$repo/scripts/gen_dataset.sh" "$dataset" "$bigdir_count" >/dev/null

echo "== fetching + building cthon04"
"$repo/scripts/fetch_cthon.sh" "$work/cthon04"

echo "== starting lightnfsd on 127.0.0.1:$nfs_port"
mkdir -p "$work/state"
cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$work/state"

[[export]]
path = "$dataset"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "root"
readonly = true

[export.local]
handles = "auto"
EOF
"$repo/build-rel/lightnfsd" --config "$work/lightnfs.toml" >"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  sleep 0.1
done

sudo CTHON_DIR="$work/cthon04" \
  "$repo/scripts/accept_m1.sh" 127.0.0.1 "$dataset" "$nfs_port" "$mount_port" "$bigdir_count"

kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero" >&2; tail -20 "$work/server.log" >&2; exit 1; }
server_pid=""
echo
echo "M1 VM mount acceptance PASSED (bigdir=$bigdir_count)"
