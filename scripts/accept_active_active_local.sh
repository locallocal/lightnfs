#!/usr/bin/env bash
# Multi-gateway active-active acceptance on loopback — no root, no cluster backend
# (design 10, plan 12 E1).  Three lightnfsd processes (gw1/gw2/gw3) share one backing
# tree and one cluster shared_dir (local backend + unsafe_skip_backend_checks); each
# listens on its own port, which is its node_address.  Two exports own-listed so gw1
# and gw2 each start with one, gw3 stands ready.  Per configuration (Release, ASAN):
#
#   status      every export Active on its first-listed live gateway, one fence.<node>
#               record each, the owner view agrees
#   v4moved     referral A->B, migration B->C (LEASE_MOVED, fs_locations, CLAIM_PREVIOUS
#               + LOCK(reclaim), verifier change, byte-verified) via lnfs_accept_client
#   roll        cluster_roll.sh evacuate gw1 then restore gw1 (D2), client uninterrupted
#   crash       kill -9 the owner of two exports; the successors in each export's list
#               pick them up (load spreads), each reclaimed and byte-verified
#   single      one gateway, three exports nodes=["gw1"]: v4rw + wtest match a single
#               gateway; status shows three Active, one fence.gw1 file (10 §10.13 gate)
#   logs        no level=error, ASAN clean, every daemon exits cleanly leaving an empty
#               fence.<node> record
#
# The backing tree and shared_dir live under the build directory, not /tmp: on tmpfs
# (no STATX_BTIME) the local backend's fallback handles are process-local.
#
# usage: accept_active_active_local.sh
#        LNFS_BUILD_DIRS="build:dbg" accept_active_active_local.sh   # existing build(s)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
p1=${LNFS_PORT1:-12319}; m1=${LNFS_MOUNT1:-12318}
p2=${LNFS_PORT2:-12329}; m2=${LNFS_MOUNT2:-12328}
p3=${LNFS_PORT3:-12339}; m3=${LNFS_MOUNT3:-12338}
fence_lease_ms=${LNFS_FENCE_LEASE_MS:-1000}
lease=${LNFS_LEASE:-3}
mkdir -p "$repo/build"
work=$(mktemp -d "$repo/build/lnfs-aa.XXXXXX")

pids=()
cleanup() {
  for p in "${pids[@]:-}"; do [[ -n $p ]] && kill -9 "$p" 2>/dev/null || true; done
  [[ ${LNFS_KEEP_WORK:-0} = 1 ]] || rm -rf "$work"
}
trap cleanup EXIT

if [[ -z ${LNFS_BUILD_DIRS:-} ]]; then
  echo "== building Release and ASAN configurations"
  cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$repo/build-rel" --target lightnfsd lightnfs-ctl lnfs_accept_client >/dev/null
  cmake -S "$repo" -B "$repo/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DLNFS_SANITIZE=address >/dev/null
  cmake --build "$repo/build-asan" --target lightnfsd lightnfs-ctl lnfs_accept_client >/dev/null
  LNFS_BUILD_DIRS="build-rel:rel build-asan:asan"
fi

# write_config <node> <state> <nfs port> <mount port> <single?>: two exports (F1 nodes
# [gw1,gw2,gw3], F2 [gw2,gw3,gw1]) normally; three [<node>] exports in single mode.
write_config() {
  local node=$1 state=$2 port=$3 mport=$4 single=${5:-0}
  {
    cat <<EOC
[server]
reactors = 1
offload_threads = 4
port = $port
mount_port = $mport
rpcbind = false
state_dir = "$state"
[protocol]
lease = "${lease}s"
courtesy_multiplier = 1
[cluster]
enabled = true
mode = "active-active"
id = "aa-accept"
shared_dir = "$work/shared"
node = "$node"
node_address = "127.0.0.1:$port"
role = "auto"
takeover = "auto"
fence_lease = "${fence_lease_ms}ms"
unsafe_skip_backend_checks = true
EOC
    if [[ $single = 1 ]]; then
      for fsid in 1 2 3; do
        cat <<EOC
[[export]]
path = "$data/e$fsid"
backend = "local"
fsid = $fsid
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false
nodes = ["gw1"]
[export.local]
handles = "auto"
EOC
      done
    else
      cat <<EOC
[[export]]
path = "$data/e1"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false
nodes = ["gw1", "gw2", "gw3"]
[export.local]
handles = "auto"
[[export]]
path = "$data/e2"
backend = "local"
fsid = 2
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false
nodes = ["gw2", "gw3", "gw1"]
[export.local]
handles = "auto"
EOC
    fi
  } > "$work/$node.toml"
}

ctl() { LIGHTNFS_CTL="$1/ctl.sock" "$2/lightnfs-ctl" "${@:3}"; }
start_node() {  # <build> <node> <state> <log> → pid
  ASAN_OPTIONS="detect_leaks=1:exitcode=42" "$1/lightnfsd" --config "$work/$2.toml" >>"$4" 2>&1 &
  echo $!
}
wait_port() { for _ in $(seq 1 "${3:-100}"); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.1; done; tail -20 "$2" >&2; return 1; }
wait_log() { for _ in $(seq 1 $3); do grep -q "$2" "$1" && return 0; sleep 0.1; done; echo "timeout: '$2' in $1" >&2; tail -20 "$1" >&2; return 1; }
wait_owner() {  # <state> <build> <fsid> <role> [tries]: poll status until that role
  for _ in $(seq 1 "${5:-100}"); do
    grep -qE "fsid=$3 role=$4 " <<<"$(ctl "$1" "$2" cluster status)" && return 0
    sleep 0.1
  done
  echo "[wait_owner] fsid $3 not $4:" >&2; ctl "$1" "$2" cluster status >&2; return 1
}
expect() { grep -qF -- "$2" <<<"$1" || { echo "[$3] expected '$2' in:"; echo "$1"; exit 1; }; }
stop_node() {  # <pid> <log> <label>
  kill "$1"
  for _ in $(seq 1 200); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$1" 2>/dev/null || grep -qE "ERROR: (Address|Leak)Sanitizer" "$2" || ! grep -q "lightnfs stopped" "$2"; then
    echo "[$3] daemon did not stop cleanly or sanitizer report" >&2; tail -40 "$2" >&2; return 1
  fi
}

run_phase() {
  local build="$repo/$1" label=$2
  local sa="$work/$label/gw1" sb="$work/$label/gw2" sc="$work/$label/gw3"
  local la="$work/$label/gw1.log" lb="$work/$label/gw2.log" lc="$work/$label/gw3.log"
  rm -rf "$work/shared" "$work/$label" "$data"
  mkdir -p "$work/shared" "$sa" "$sb" "$sc" "$data/e1" "$data/e2" "$data/e3"
  export LNFS_CTL_SOCKETS="gw1=$sa/ctl.sock,gw2=$sb/ctl.sock,gw3=$sc/ctl.sock"

  echo "== [$label] start gw1/gw2/gw3"
  write_config gw1 "$sa" "$p1" "$m1"; write_config gw2 "$sb" "$p2" "$m2"; write_config gw3 "$sc" "$p3" "$m3"
  pids=(); pids+=("$(start_node "$build" gw1 "$sa" "$la")")
  pids+=("$(start_node "$build" gw2 "$sb" "$lb")"); pids+=("$(start_node "$build" gw3 "$sc" "$lc")")
  wait_port "$p1" "$la"; wait_port "$p2" "$lb"; wait_port "$p3" "$lc"
  wait_log "$la" "gw1 serves fsid 1" 60; wait_log "$lb" "gw2 serves fsid 2" 60
  sleep 1.5
  local st; st=$(ctl "$sa" "$build" cluster status)
  expect "$st" "fsid=1 role=active nodes=gw1,gw2,gw3 owner=gw1 address=127.0.0.1:$p1" "$label"
  expect "$st" "fsid=2 role=remote nodes=gw2,gw3,gw1 owner=gw2 address=127.0.0.1:$p2" "$label"
  echo "   gw1: $(grep -c 'role=active' <<<"$st") active of 2"

  echo "== [$label] v4moved: referral gw1->gw2, migration gw2->gw3 (fsid 2)"
  local mig="LIGHTNFS_CTL=$sb/ctl.sock $build/lightnfs-ctl cluster migrate 2 gw3"
  "$build/lnfs_accept_client" v4moved 127.0.0.1 "$p1" "$p2" "$p3" "$data/e2" "$data/e2" "$mig"
  sleep 1.5
  ctl "$sc" "$build" cluster status | grep -qF "fsid=2 role=active nodes=gw2,gw3,gw1 owner=gw3" ||
    { echo "[$label] fsid 2 not on gw3 after migration"; exit 1; }
  # hand fsid 2 back to gw2 for the roll segment.
  ctl "$sc" "$build" cluster migrate 2 gw2 >/dev/null
  wait_owner "$sb" "$build" 2 active 100

  echo "== [$label] roll: evacuate gw1 then restore"
  "$repo/scripts/cluster_roll.sh" --ctl "$build/lightnfs-ctl" evacuate gw1
  ctl "$sa" "$build" cluster status | grep -qE "fsid=1 role=(remote|unowned)" ||
    { echo "[$label] gw1 still owns fsid 1 after evacuate"; exit 1; }
  "$repo/scripts/cluster_roll.sh" --ctl "$build/lightnfs-ctl" restore gw1
  wait_owner "$sa" "$build" 1 active 100

  echo "== [$label] crash: kill -9 gw1, its exports spread to the live successors"
  wait_owner "$sa" "$build" 1 active 100
  "$build/lnfs_accept_client" v4rw 127.0.0.1 "$p1" "$m1" "$data/e1" "$data/e1" >/dev/null
  rm -f "$data/e1/v4rw.bin"
  kill -9 "${pids[0]}"; for _ in $(seq 1 100); do kill -0 "${pids[0]}" 2>/dev/null || break; sleep 0.1; done
  pids[0]=""
  # fsid 1 (nodes gw1,gw2,gw3) → gw2; fsid 2 is gw2's already.  Wait for the takeover.
  wait_owner "$sb" "$build" 1 active $((10 * fence_lease_ms / 100))
  "$build/lnfs_accept_client" wtest 127.0.0.1 "$p2" "$m2" "$data/e1" "$data/e1" >/dev/null
  rm -rf "$data/e1/wtest"

  echo "== [$label] shutdown gw2/gw3"
  stop_node "${pids[1]}" "$lb" "$label/gw2"; stop_node "${pids[2]}" "$lc" "$label/gw3"; pids=()
  for f in "$la" "$lb" "$lc"; do
    [[ -f $f ]] && grep -h "level=error" "$f" && { echo "[$label] error lines above" >&2; exit 1; }
  done || true
  # gw2/gw3 left an empty fence record; gw1 crashed (record stays, aged out).
  for n in gw2 gw3; do
    [[ -f "$work/shared/fence.$n" ]] || { echo "[$label] fence.$n missing after exit"; exit 1; }
  done
}

single_phase() {
  local build="$repo/$1" label="$2-single"
  local sa="$work/$label/gw1" la="$work/$label/gw1.log"
  rm -rf "$work/shared" "$work/$label" "$data"
  mkdir -p "$work/shared" "$sa" "$data/e1" "$data/e2" "$data/e3"
  echo "== [$label] single gateway, three exports nodes=[gw1] (10 §10.13 degeneration)"
  write_config gw1 "$sa" "$p1" "$m1" 1
  pids=("$(start_node "$build" gw1 "$sa" "$la")")
  wait_port "$p1" "$la"
  wait_log "$la" "gw1 serves fsid 3" 60; sleep 1.2
  local st; st=$(ctl "$sa" "$build" cluster status)
  [[ $(grep -c 'role=active' <<<"$st") -eq 3 ]] || { echo "[$label] not three Active"; echo "$st"; exit 1; }
  ls "$work/shared"/fence.* | grep -qx "$work/shared/fence.gw1" &&
    [[ $(ls "$work/shared"/fence.* | wc -l) -eq 1 ]] || { echo "[$label] not exactly one fence file"; exit 1; }
  "$build/lnfs_accept_client" v4rw 127.0.0.1 "$p1" "$m1" "$data/e1" "$data/e1" >/dev/null
  "$build/lnfs_accept_client" wtest 127.0.0.1 "$p1" "$m1" "$data/e2" "$data/e2" >/dev/null
  rm -f "$data/e1/v4rw.bin"; rm -rf "$data/e2/wtest"
  stop_node "${pids[0]}" "$la" "$label"; pids=()
  grep -h "level=error" "$la" && { echo "[$label] error lines above" >&2; exit 1; }
  echo "   single-gateway degeneration OK"
}

data="$work/data"
for entry in $LNFS_BUILD_DIRS; do
  run_phase "${entry%%:*}" "${entry##*:}"
  single_phase "${entry%%:*}" "${entry##*:}"
done

echo
echo "multi-gateway active-active loopback acceptance PASSED"
