#!/usr/bin/env bash
# Weekly fault-injection run (development plan §9 "故障注入" row): no root, loopback.
#
#   crash-loop   N x (write under load -> kill -9 -> restart -> client resend converges,
#                verifier changes) using the M2 crash-write/crash-recover pair
#   fsync-eio    server started with LNFS_FAULT_FSYNC_EIO=1: the injected fsync failure
#                surfaces as NFS3ERR_IO and stays sticky (design 06 §6.2); unrelated
#                files unaffected
#   client-kill  v4.1 holder vanishes without CLOSE (the "client VM kill" stand-in):
#                courtesy conflict + timeout reclaims counted via lightnfs-ctl
#   v4-restart   kill -9 with open state held: CLAIM_PREVIOUS reclaim inside grace
#
# usage: fault_inject.sh [CRASH_ITERATIONS]   (default 5; nightly/weekly uses 20)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
iters=${1:-5}
nfs_port=${LNFS_NFS_PORT:-12319}
mount_port=${LNFS_MOUNT_PORT:-12318}
lease=${LNFS_LEASE:-3}
work=$(mktemp -d "/tmp/lnfs-fault.XXXXXX")
build="$repo/build-rel"
client="$build/lnfs_accept_client"
data="$work/data"
state="$work/state"
log="$work/server.log"

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building Release"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null

mkdir -p "$data" "$state"
cat > "$work/lightnfs.toml" <<EOC
[server]
reactors = 0
offload_threads = 4
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$state"

[protocol]
lease = "${lease}s"
courtesy_multiplier = 1

[[export]]
path = "$data"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.local]
handles = "auto"
EOC

start_server() {  # env passed through (LNFS_FAULT_*)
  "$build/lightnfsd" --config "$work/lightnfs.toml" >>"$log" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && return 0
    sleep 0.1
  done
  tail -20 "$log" >&2
  return 1
}
stop9() { kill -9 "$server_pid"; while kill -0 "$server_pid" 2>/dev/null; do sleep 0.05; done; server_pid=""; }
ctl() { LIGHTNFS_CTL="$state/ctl.sock" "$build/lightnfs-ctl" "$@"; }

echo "== crash-loop: $iters x write / kill -9 / restart / recover"
start_server
for i in $(seq 1 "$iters"); do
  "$client" crash-write 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$work/crash.state" >/dev/null
  stop9
  start_server
  "$client" crash-recover 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data" "$work/crash.state" >/dev/null
  echo "   iteration $i recovered"
done
kill "$server_pid"; wait "$server_pid" || true; server_pid=""

echo "== fsync-eio: injected EIO -> NFS3ERR_IO, sticky poison"
LNFS_FAULT_FSYNC_EIO=1 start_server
"$client" fsync-eio 127.0.0.1 "$nfs_port" "$mount_port" "$data"
kill "$server_pid"; wait "$server_pid" || true; server_pid=""
rm -rf "$data/fault"

echo "== client-kill: v4.1 holder vanishes -> courtesy conflict + timeout reclaims"
start_server
"$client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$lease"
sleep $((2 * lease + 3))
st=$(ctl state | head -1)
echo "   $st"
grep -q "reclaim_conflict=1" <<<"$st" || { echo "conflict reclaim not counted" >&2; exit 1; }
grep -q "reclaim_timeout=1" <<<"$st" || { echo "timeout reclaim not counted" >&2; exit 1; }

echo "== v4-restart: kill -9 with open state held, CLAIM_PREVIOUS inside grace"
cat > "$work/restart.sh" <<EOC
#!/usr/bin/env bash
set -e
kill -9 $server_pid
while kill -0 $server_pid 2>/dev/null; do sleep 0.1; done
"$build/lightnfsd" --config "$work/lightnfs.toml" >>"$log" 2>&1 &
echo \$! > "$work/server.pid"
for _ in \$(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && exit 0
  sleep 0.1
done
exit 1
EOC
chmod +x "$work/restart.sh"
"$client" v4reclaim 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data" "$work/restart.sh"
server_pid=$(cat "$work/server.pid")
grep -q "grace period armed" "$log" || { echo "no grace after restart" >&2; exit 1; }
kill "$server_pid"
for _ in $(seq 1 100); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.1; done
server_pid=""
grep -q "lightnfs stopped" "$log" || { echo "server did not stop cleanly" >&2; exit 1; }

echo
echo "fault injection PASSED ($iters crash iterations, fsync EIO, client kill, v4 restart)"
