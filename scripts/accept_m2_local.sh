#!/usr/bin/env bash
# Phase-2 (M2+M3) loopback acceptance — no root, no kernel mount (development plan
# §4.6, the locally-runnable half).  Builds Release + ASAN, then against a real
# lightnfsd on loopback TCP:
#
#   unit         full ctest suite in both configurations
#   wtest        create/write(3 stability levels)/commit/setattr/exclusive-create
#                replay/namespace ops/DRC retransmission — byte-verified vs backing dir
#   crash        kill -9 + restart: boot-epoch verifier changes, client resend converges
#   connstorm    10k concurrent connections + beyond-inflight pipelining (backpressure)
#   tools        lightnfs-ctl (ping/metrics/fdcache/drc/dump-errors) + metrics HTTP
#   asan soak    the same rw workload + read stress under ASAN, leak-checked exit
#
# The kernel-mount half (cthon04 basic/general/special, fsx) is scripts/accept_m2_vm.sh.
#
# usage: accept_m2_local.sh [ASAN_STRESS_SECONDS]
set -euo pipefail

stress_secs=${1:-120}
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12099}
mount_port=${LNFS_MOUNT_PORT:-12098}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m2.XXXXXX")

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building Release and ASAN configurations"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lnfs_accept_client lightnfs-ctl lnfs_tests >/dev/null
cmake -S "$repo" -B "$repo/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLNFS_SANITIZE=address >/dev/null
cmake --build "$repo/build-asan" --target lightnfsd lnfs_accept_client lnfs_tests >/dev/null

echo "== unit tests (Release + ASAN)"
ctest --test-dir "$repo/build-rel" --output-on-failure >/dev/null
ctest --test-dir "$repo/build-asan" --output-on-failure >/dev/null

write_config() {  # $1 data dir, $2 state dir
  cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$2"
max_connections = 20000
per_peer_limit = 20000
metrics_port = $((nfs_port + 1))

[protocol]
drc_ttl = "60s"
drc_mem = "16MiB"

[[export]]
path = "$1"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.local]
handles = "auto"
EOF
}

wait_port() {
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && return 0
    sleep 0.1
  done
  echo "server did not start" >&2
  tail -20 "$1" >&2
  return 1
}

start_server() {  # $1 build dir, $2 log
  ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=42" \
    "$1/lightnfsd" --config "$work/lightnfs.toml" >>"$2" 2>&1 &
  server_pid=$!
  wait_port "$2"
}

stop_server() {  # $1 log; graceful, then verify clean sanitizer exit
  kill "$server_pid"
  local rc=0
  wait "$server_pid" || rc=$?
  server_pid=""
  if [[ $rc -ne 0 ]] || grep -qE "ERROR: (Address|Leak)Sanitizer" "$1"; then
    echo "server exit rc=$rc or sanitizer report:" >&2
    tail -40 "$1" >&2
    return 1
  fi
}

run_phase() {  # $1 build label, $2 stress secs
  local build="$repo/$1" label=$1 secs=$2
  local data="$work/data-$label" state="/tmp/lnfs-m2-state-$label" log="$work/server-$label.log"
  rm -rf "$data" "$state"
  mkdir -p "$data" "$state"
  write_config "$data" "$state"
  local client="$build/lnfs_accept_client"

  echo "== [$label] rw acceptance (wtest)"
  start_server "$build" "$log"
  "$client" wtest 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"

  echo "== [$label] kill -9 crash recovery"
  "$client" crash-write 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$work/crash-$label.state"
  kill -9 "$server_pid"
  server_pid=""
  sleep 0.3
  start_server "$build" "$log"
  "$client" crash-recover 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data" \
    "$work/crash-$label.state"

  if [[ $label == build-rel ]]; then
    echo "== [$label] connection storm (10k conns) + backpressure"
    (ulimit -n 30000 2>/dev/null || true
     "$client" connstorm 127.0.0.1 "$nfs_port" "$mount_port" 10000 512)

    echo "== [$label] admin tools"
    export LIGHTNFS_CTL="$state/ctl.sock"
    "$repo/build-rel/lightnfs-ctl" ping >/dev/null
    "$repo/build-rel/lightnfs-ctl" metrics | grep -q lightnfs_v3_calls_total
    "$repo/build-rel/lightnfs-ctl" fdcache >/dev/null
    "$repo/build-rel/lightnfs-ctl" drc | grep -q inserts=
    curl -sf "http://127.0.0.1:$((nfs_port + 1))/metrics" | grep -q lightnfs_drc_inserts_total
  else
    echo "== [$label] concurrent rw stress ${secs}s (leak soak)"
    head -c 8388608 /dev/urandom > "$data/soak.bin"
    "$client" stress 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data" soak.bin 8 32 "$secs"
  fi

  echo "== [$label] graceful shutdown + sanitizer check"
  stop_server "$log"
}

run_phase build-rel 0
run_phase build-asan "$stress_secs"

echo
echo "M2 loopback acceptance PASSED (Release + ASAN, ASAN soak ${stress_secs}s)"
