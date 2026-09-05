#!/usr/bin/env bash
# Multi-gateway failover acceptance on loopback — no root, no cluster backend
# (design 09, plan 10 E1).  Two lightnfsd processes share one backing tree and one
# cluster shared_dir (local backend + unsafe_skip_backend_checks); A starts active,
# B stands by (takeover = manual) and does not listen.  Per configuration (Release,
# then ASAN):
#
#   status      A active / B standby, epoch, B not listening
#   v3 before   wtest (NFSv3 read-write, byte-verified) against A
#   v4failover  A holds OPEN + LOCK + unstable WRITE → kill -9 A → `lightnfs-ctl -s B
#               cluster takeover` (retried until A's fence lapses; no --force) → on B:
#               BADSESSION for A's session, same server_owner/scope, clientid epoch +1,
#               STALE_STATEID, GRACE for a plain OPEN, CLAIM_PREVIOUS + LOCK(reclaim),
#               write verifier changed → data re-sent and byte-verified, early grace exit
#   status      B active, epoch +1, takeovers=1, activation < 1s (plan 10 §9.6)
#   v3 after    wtest against B
#   split-brain A restarted as a standby; the fence file is rewritten by hand to name
#               A: B loses the fence on its next renew and drains itself within
#               3 × fence_lease (fence_lost_total = 1, port closed), A sees its own
#               name on the record and takes over again (epoch +2), releasing the
#               fence on exit
#   logs        no `level=error` line, ASAN clean, both daemons stop cleanly
#
# The backing tree and shared_dir live under the build directory, not /tmp: on tmpfs
# (no STATX_BTIME) the local backend's fallback handles are process-local and B would
# answer ESTALE for A's filehandles.
#
# usage: accept_failover_local.sh
#        LNFS_BUILD_DIRS="build:dbg" accept_failover_local.sh   # existing build(s), no rebuild
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
port_a=${LNFS_PORT_A:-12219}
mount_a=${LNFS_MOUNT_A:-12218}
port_b=${LNFS_PORT_B:-12229}
mount_b=${LNFS_MOUNT_B:-12228}
lease=${LNFS_LEASE:-3}
fence_lease_ms=${LNFS_FENCE_LEASE_MS:-1000}
mkdir -p "$repo/build"
work=$(mktemp -d "$repo/build/lnfs-failover.XXXXXX")

pid_a=""
pid_b=""
cleanup() {
  for p in $pid_a $pid_b; do
    kill -9 "$p" 2>/dev/null || true
  done
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

write_config() {  # $1 node (a|b), $2 state dir, $3 nfs port, $4 mount port, $5 role, $6 takeover
  cat > "$work/$1.toml" <<EOC
[server]
reactors = 1
offload_threads = 4
port = $3
mount_port = $4
rpcbind = false
state_dir = "$2"

[protocol]
lease = "${lease}s"
courtesy_multiplier = 1

[cluster]
enabled = true
id = "failover-accept-cluster"
shared_dir = "$work/shared"
node = "gw-$1"
role = "$5"
takeover = "$6"
fence_lease = "${fence_lease_ms}ms"
unsafe_skip_backend_checks = true

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
}

wait_port() {  # $1 port, $2 log, $3 tries (0.1s each)
  for _ in $(seq 1 "${3:-100}"); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
    sleep 0.1
  done
  tail -20 "$2" >&2
  return 1
}

port_closed() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

start_node() {  # $1 build dir, $2 node, $3 log → pid on stdout
  ASAN_OPTIONS="detect_leaks=1:exitcode=42" \
    "$1/lightnfsd" --config "$work/$2.toml" >>"$3" 2>&1 &
  echo $!
}

ctl() { LIGHTNFS_CTL="$1/ctl.sock" "$2/lightnfs-ctl" "${@:3}"; }

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }  # ms (some `date` ignore %3N)

field() { sed -nE "s/.*\b$2=([^ ]*).*/\1/p" <<<"$1"; }  # $1 status line, $2 key

expect_field() {  # $1 label, $2 status line, $3 key, $4 expected
  local got
  got=$(field "$2" "$3")
  if [[ $got != "$4" ]]; then
    echo "[$1] expected $3=$4, got: $2" >&2
    exit 1
  fi
}

wait_status() {  # $1 state dir, $2 build, $3 key, $4 value, $5 timeout ms → status line
  local deadline=$(( $(now_ms) + $5 )) st
  while :; do
    st=$(ctl "$1" "$2" cluster status)
    [[ $(field "$st" "$3") == "$4" ]] && { echo "$st"; return 0; }
    (( $(now_ms) < deadline )) || { echo "$st"; return 1; }
    sleep 0.1
  done
}

stop_node() {  # $1 pid, $2 log, $3 label
  kill "$1"
  local alive=1
  for _ in $(seq 1 200); do
    if ! kill -0 "$1" 2>/dev/null; then alive=0; break; fi
    sleep 0.1
  done
  if [[ $alive -ne 0 ]] || grep -qE "ERROR: (Address|Leak)Sanitizer" "$2" ||
     ! grep -q "lightnfs stopped" "$2"; then
    echo "[$3] daemon did not stop cleanly or sanitizer report" >&2
    tail -40 "$2" >&2
    return 1
  fi
}

run_phase() {  # $1 build dir, $2 label
  local build="$repo/$1" label=$2
  local state_a="$work/$label/a" state_b="$work/$label/b"
  local log_a="$work/$label/a.log" log_b="$work/$label/b.log"
  rm -rf "$work/shared" "$work/$label" "$data/wtest" "$data/failover.bin"
  mkdir -p "$work/shared" "$state_a" "$state_b"
  write_config a "$state_a" "$port_a" "$mount_a" active auto
  write_config b "$state_b" "$port_b" "$mount_b" standby manual

  echo "== [$label] start A (active) and B (standby, manual takeover)"
  pid_a=$(start_node "$build" a "$log_a")
  pid_b=$(start_node "$build" b "$log_b")
  wait_port "$port_a" "$log_a"
  for _ in $(seq 1 50); do [[ -S "$state_b/ctl.sock" ]] && break; sleep 0.1; done
  local st_a st_b epoch_a
  st_a=$(ctl "$state_a" "$build" cluster status)
  # B polls the fence once per fence_lease: give it a tick to see A's record.
  st_b=$(wait_status "$state_b" "$build" fence_owner gw-a $((3 * fence_lease_ms))) ||
    { echo "[$label] B never saw A's fence: $st_b" >&2; exit 1; }
  echo "   A: $st_a"
  echo "   B: $st_b"
  expect_field "$label" "$st_a" role active
  expect_field "$label" "$st_b" role standby
  epoch_a=$(field "$st_a" epoch)
  port_closed "$port_b" || { echo "[$label] standby B is listening" >&2; exit 1; }

  echo "== [$label] v3 wtest against A"
  "$build/lnfs_accept_client" wtest 127.0.0.1 "$port_a" "$mount_a" "$data" "$data"
  rm -rf "$data/wtest"

  echo "== [$label] v4failover: kill -9 A, B takes over, state reclaimed on B"
  cat > "$work/takeover-$label.sh" <<EOC
#!/usr/bin/env bash
set -e
kill -9 $pid_a
while kill -0 $pid_a 2>/dev/null; do sleep 0.05; done
# Manual policy, no --force: the request is refused while A's fence is still live and
# succeeds once it lapses (3 × fence_lease + skew) — the realistic operator flow.
for _ in \$(seq 1 100); do
  out=\$(LIGHTNFS_CTL="$state_b/ctl.sock" "$build/lightnfs-ctl" cluster takeover --json)
  grep -q '"takeover":true' <<<"\$out" && break
  sleep 0.1
done
grep -q '"takeover":true' <<<"\$out" || { echo "takeover never accepted: \$out" >&2; exit 1; }
for _ in \$(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$port_b") 2>/dev/null && exit 0
  sleep 0.05
done
echo "B never listened" >&2
exit 1
EOC
  chmod +x "$work/takeover-$label.sh"
  "$build/lnfs_accept_client" v4failover 127.0.0.1 "$port_a" "$port_b" "$data" "$data" \
    "$work/takeover-$label.sh"
  pid_a=""
  st_b=$(ctl "$state_b" "$build" cluster status)
  echo "   B: $st_b"
  expect_field "$label" "$st_b" role active
  expect_field "$label" "$st_b" epoch "$((epoch_a + 1))"
  expect_field "$label" "$st_b" takeovers 1
  expect_field "$label" "$st_b" fence_owner gw-b
  local act_ms
  act_ms=$(field "$st_b" last_activation_ms)
  (( act_ms < 1000 )) || { echo "[$label] activation took ${act_ms} ms (limit 1000)" >&2; exit 1; }
  grep -q "grace period armed" "$log_b" || { echo "[$label] B never entered grace" >&2; exit 1; }
  grep -q "leaving grace early" "$log_b" || { echo "[$label] RECLAIM_COMPLETE did not end grace on B" >&2; exit 1; }
  # Capture then grep: `ctl | grep -q` under `pipefail` fails when grep matches and
  # closes the pipe early, leaving lightnfs-ctl with a SIGPIPE.
  local metrics
  metrics=$(ctl "$state_b" "$build" metrics)
  grep -q "^lightnfs_cluster_takeovers_total 1$" <<<"$metrics" ||
    { echo "[$label] takeovers_total metric not 1" >&2; exit 1; }

  echo "== [$label] v3 wtest against B"
  "$build/lnfs_accept_client" wtest 127.0.0.1 "$port_b" "$mount_b" "$data" "$data"
  rm -rf "$data/wtest"

  echo "== [$label] split-brain: A back as standby, fence rewritten to name A"
  pid_a=$(start_node "$build" a "$log_a")
  for _ in $(seq 1 50); do [[ -S "$state_a/ctl.sock" ]] && break; sleep 0.1; done
  st_a=$(wait_status "$state_a" "$build" fence_owner gw-b $((3 * fence_lease_ms))) ||
    { echo "[$label] restarted A never saw B's fence: $st_a" >&2; exit 1; }
  echo "   A: $st_a"
  expect_field "$label" "$st_a" role standby
  # Someone else's hand on the fence (operator error, a partitioned peer): B must
  # notice on its next renew and drain — the second line of defence behind the VIP.
  printf '%s %s gw-a\n' "$((epoch_a + 2))" "$(( $(now_ms) + 60000 ))" > "$work/shared/fence.tmp"
  mv "$work/shared/fence.tmp" "$work/shared/fence"
  local drained=0 deadline=$(( $(now_ms) + 3 * fence_lease_ms + 2000 ))
  while (( $(now_ms) < deadline )); do
    st_b=$(ctl "$state_b" "$build" cluster status)
    if [[ $(field "$st_b" role) == standby ]]; then drained=1; break; fi
    sleep 0.1
  done
  echo "   B: $st_b"
  (( drained )) || { echo "[$label] B did not drain after losing the fence" >&2; exit 1; }
  expect_field "$label" "$st_b" fence_lost 1
  metrics=$(ctl "$state_b" "$build" metrics)
  grep -q "^lightnfs_cluster_fence_lost_total 1$" <<<"$metrics" ||
    { echo "[$label] fence_lost_total metric not 1" >&2; exit 1; }
  for _ in $(seq 1 50); do port_closed "$port_b" && break; sleep 0.1; done
  port_closed "$port_b" || { echo "[$label] drained B still listening" >&2; exit 1; }
  # A's own name on the record = its previous incarnation: automatic takeover.
  wait_port "$port_a" "$log_a" 100
  st_a=$(ctl "$state_a" "$build" cluster status)
  echo "   A: $st_a"
  expect_field "$label" "$st_a" role active
  expect_field "$label" "$st_a" epoch "$((epoch_a + 2))"
  "$build/lnfs_accept_client" wtest 127.0.0.1 "$port_a" "$mount_a" "$data" "$data"
  rm -rf "$data/wtest"

  echo "== [$label] graceful shutdown + log check"
  stop_node "$pid_b" "$log_b" "$label/B"
  pid_b=""
  stop_node "$pid_a" "$log_a" "$label/A"
  pid_a=""
  grep -q "fence released on exit" "$log_a" || { echo "[$label] A did not release the fence" >&2; exit 1; }
  [[ -e "$work/shared/fence" ]] && { echo "[$label] fence file left behind" >&2; exit 1; }
  if grep -h "level=error" "$log_a" "$log_b"; then
    echo "[$label] error-level log lines above" >&2
    exit 1
  fi
}

echo "== dataset"
data="$work/data"
mkdir -p "$data"
echo "failover acceptance" > "$data/hello.txt"
head -c 200003 /dev/urandom > "$data/big.bin"

for entry in $LNFS_BUILD_DIRS; do  # "dir:label" pairs
  run_phase "${entry%%:*}" "${entry##*:}"
done

echo
echo "multi-gateway failover loopback acceptance PASSED"
