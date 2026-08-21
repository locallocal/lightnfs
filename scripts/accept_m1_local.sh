#!/usr/bin/env bash
# M1 loopback acceptance (development plan §3.5) — no root, no kernel mount.
#
# Builds lightnfsd (Release + ASAN), generates the acceptance dataset, starts the real
# server on loopback TCP and drives it with the userspace NFSv3 client
# (lnfs_accept_client):
#   walk    full traversal + content/symlink verification + negative checks
#   bigdir  100k-entry READDIR pagination: no duplicates, no omissions
#   stress  concurrent pipelined READs, payload-verified (ASAN pass = leak soak)
#
# The kernel-mount half of §3.5 (mount -o vers=3, ls -lR/cat/md5sum -c, cthon) needs
# privileges: run scripts/accept_m1_container.sh on a host with docker, or
# scripts/accept_m1.sh inside any VM that can mount NFS.
#
# usage: accept_m1_local.sh [ASAN_STRESS_SECONDS] [BIGDIR_COUNT]
set -euo pipefail

stress_secs=${1:-300}
bigdir_count=${2:-100000}
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12049}
mount_port=${LNFS_MOUNT_PORT:-12048}
work=${LNFS_ACCEPT_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/lightnfs-accept.XXXXXX")}
dataset="$work/dataset"

echo "== work dir: $work"

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building Release and ASAN configurations"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lnfs_accept_client >/dev/null
cmake -S "$repo" -B "$repo/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLNFS_SANITIZE=address >/dev/null
cmake --build "$repo/build-asan" --target lightnfsd lnfs_accept_client >/dev/null

echo "== generating dataset ($bigdir_count bigdir entries)"
"$repo/scripts/gen_dataset.sh" "$dataset" "$bigdir_count" >/dev/null

write_config() {  # $1 = state dir
  cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$1"

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
}

wait_port() {
  for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null; then exec 3>&-; return 0; fi
    sleep 0.1
  done
  echo "server did not start (see $1)" >&2
  tail -20 "$1" >&2
  return 1
}

run_phase() {  # $1 = build dir label, $2 = stress seconds
  local build="$repo/$1" label=$1 secs=$2
  local state="$work/state-$label" log="$work/server-$label.log"
  mkdir -p "$state"
  write_config "$state"
  echo "== [$label] starting lightnfsd"
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=42" \
    "$build/lightnfsd" --config "$work/lightnfs.toml" >"$log" 2>&1 &
  server_pid=$!
  wait_port "$log"

  local client="$build/lnfs_accept_client"
  echo "== [$label] walk (full traversal + content verification)"
  "$client" walk 127.0.0.1 "$nfs_port" "$mount_port" "$dataset" "$dataset" ro
  echo "== [$label] bigdir ($bigdir_count entries)"
  "$client" bigdir 127.0.0.1 "$nfs_port" "$mount_port" "$dataset" "$dataset" \
    bigdir "$bigdir_count"
  echo "== [$label] stress (8 conns x 32 pipeline, ${secs}s)"
  "$client" stress 127.0.0.1 "$nfs_port" "$mount_port" "$dataset" "$dataset" \
    tree/f_8388608.bin 8 32 "$secs"

  echo "== [$label] graceful shutdown + sanitizer check"
  kill "$server_pid"
  local rc=0
  wait "$server_pid" || rc=$?
  server_pid=""
  if [[ $rc -ne 0 ]]; then
    echo "[$label] server exited with code $rc" >&2
    tail -40 "$log" >&2
    return 1
  fi
  if grep -qE "ERROR: (Address|Leak)Sanitizer|SUMMARY: .*Sanitizer" "$log"; then
    echo "[$label] sanitizer reported errors:" >&2
    grep -A5 -E "Sanitizer" "$log" >&2
    return 1
  fi
}

run_phase build-rel 30
run_phase build-asan "$stress_secs"

echo
echo "M1 loopback acceptance PASSED (Release + ASAN, bigdir=$bigdir_count, \
ASAN stress ${stress_secs}s)"
