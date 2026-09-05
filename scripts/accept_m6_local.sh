#!/usr/bin/env bash
# Phase-6 (M8 item 1) loopback acceptance — no root, no kernel mount (development plan
# §8, the locally-runnable half).  Against a real lightnfsd on loopback TCP:
#
#   unit        full ctest suite (Release + ASAN)
#   v42         vers=4.2 sweets: DEALLOCATE/SEEK/ALLOCATE mirrored on the backing file
#               (sizes, zeroes, lseek(SEEK_HOLE) agreement), whole-file + ranged COPY
#               byte-verified, CLONE honored or NOTSUPP per export fs, OPENMODE/INVAL/
#               NXIO discipline, the 4.2 opcodes OP_ILLEGAL at minorversion 1
#   v4rw        vers=4.1 read-write: OPEN(CREATE)/WRITE(UNSTABLE)+COMMIT verifier/
#               READ-back/SETATTR truncate, share reservations across two clients,
#               OPENMODE/LOCKED/OLD_STATEID discipline, OPEN_DOWNGRADE, CREATE dir/
#               RENAME/LINK/REMOVE — every step mirrored on the backing tree
#   v4lock      byte-range LOCK/LOCKT/LOCKU across two clients: new/existing lock owner,
#               DENIED with holder info, upgrade + LOCKU split, CLOSE releases locks
#   v4walk/walk phase-3 read paths (v4.1 + v3) still byte-verified on the same tree
#   reclaim     server restart with open state held (07 §7.5 scenario): kill -9,
#               restart, CLAIM_PREVIOUS inside grace, data intact, STALE_STATEID/GRACE/
#               NO_GRACE gates, RECLAIM_COMPLETE early grace exit
#   courtesy    lease expiry (07 §7.4): holder vanishes → SHARE_DENIED inside the lease,
#               conflict reclaim after it; a second vanished holder is reclaimed by the
#               courtesy timeout — both counted via lightnfs-ctl state
#   ctl         state table dump + forced client reclaim (expire-client)
#   pynfs       4.1 currentstateid + courteous (both drive LOCK) + secinfo_no_name +
#               SEC1/SEC2 (named SECINFO), plus open/rename and the phase-3 session groups
#   asan        the protocol phases again under ASAN, leak-checked graceful exit
#
# usage: accept_m6_local.sh
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12119}
mount_port=${LNFS_MOUNT_PORT:-12118}
lease=${LNFS_LEASE:-3}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m6.XXXXXX")

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building Release and ASAN configurations"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" >/dev/null  # every target: ctest also lists the fuzz_regress_* binaries
cmake -S "$repo" -B "$repo/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLNFS_SANITIZE=address >/dev/null
cmake --build "$repo/build-asan" >/dev/null

echo "== unit tests (Release + ASAN)"
ctest --test-dir "$repo/build-rel" --output-on-failure >/dev/null
ctest --test-dir "$repo/build-asan" --output-on-failure >/dev/null

echo "== dataset"
data="$work/data"
mkdir -p "$data/sub/deep" "$data/tmp" "$data/tree/dir"
echo "v4 acceptance" > "$data/hello.txt"
head -c 2000003 /dev/urandom > "$data/big.bin"
head -c 40000 /dev/urandom > "$data/sub/deep/nested.bin"
ln -s hello.txt "$data/lnk"
echo data > "$data/tree/file"
ln -sf file "$data/tree/link"
mkfifo "$data/tree/fifo" 2>/dev/null || true
python3 - "$data/tree/socket" <<'PYEOF' 2>/dev/null || true
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
PYEOF

write_config() {  # $1 = state dir
  cat > "$work/lightnfs.toml" <<EOC
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$1"

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
}

wait_port() {
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && return 0
    sleep 0.1
  done
  tail -20 "$2" >&2
  return 1
}

start_server() {  # $1 build dir, $2 log
  ASAN_OPTIONS="detect_leaks=1:exitcode=42" \
    "$1/lightnfsd" --config "$work/lightnfs.toml" >>"$2" 2>&1 &
  server_pid=$!
  wait_port "$nfs_port" "$2"
}

ctl() { LIGHTNFS_CTL="$1/ctl.sock" "$2/lightnfs-ctl" "${@:3}"; }

run_phase() {  # $1 build dir, $2 label
  local build="$repo/$1" label=$2
  local state="/tmp/lnfs-m6-state-$label" log="$work/server-$label.log"
  rm -rf "$state" "$data"/v4rw.bin "$data"/reclaim.bin "$data"/courtesy.bin "$data"/timeout.bin \
    "$data"/v42src.bin "$data"/v42dst.bin "$data"/v42clone.bin "$data"/v4lock.bin
  mkdir -p "$state"
  write_config "$state"
  start_server "$build" "$log"

  echo "== [$label] v42 (vers=4.2 SEEK/ALLOCATE/DEALLOCATE/COPY/CLONE, byte-verified)"
  "$build/lnfs_accept_client" v42 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
  grep -q "v4.2 capabilities" "$log" || { echo "capability probe not logged" >&2; exit 1; }
  echo "== [$label] v4rw (vers=4.1 read-write, byte-verified against backing)"
  "$build/lnfs_accept_client" v4rw 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
  echo "== [$label] v4lock (byte-range LOCK/LOCKT/LOCKU across two clients)"
  "$build/lnfs_accept_client" v4lock 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
  echo "== [$label] v4walk + v3 walk (read paths unchanged)"
  "$build/lnfs_accept_client" v4walk 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
  "$build/lnfs_accept_client" walk 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"

  echo "== [$label] restart reclaim (open state held across kill -9 + restart)"
  # The restart command runs from inside the client between its two phases.
  cat > "$work/restart-$label.sh" <<EOC
#!/usr/bin/env bash
set -e
kill -9 $server_pid
while kill -0 $server_pid 2>/dev/null; do sleep 0.1; done
ASAN_OPTIONS="detect_leaks=1:exitcode=42" "$build/lightnfsd" --config "$work/lightnfs.toml" >>"$log" 2>&1 &
echo \$! > "$work/server-$label.pid"
for _ in \$(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && exit 0
  sleep 0.1
done
exit 1
EOC
  chmod +x "$work/restart-$label.sh"
  "$build/lnfs_accept_client" v4reclaim 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data" \
    "$work/restart-$label.sh"
  server_pid=$(cat "$work/server-$label.pid")
  grep -q "grace period armed" "$log" || { echo "server never entered grace" >&2; exit 1; }
  grep -q "leaving grace early" "$log" || { echo "RECLAIM_COMPLETE did not end grace" >&2; exit 1; }

  echo "== [$label] courtesy: lease expiry → conflict reclaim + timeout reclaim"
  "$build/lnfs_accept_client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$lease"
  st=$(ctl "$state" "$build" state | head -1)
  echo "   $st"
  grep -q "reclaim_conflict=1" <<<"$st" || { echo "conflict reclaim not counted" >&2; exit 1; }
  # timeout path: lease + courtesy window (1 × lease) + scanner slack
  sleep $((2 * lease + 3))
  st=$(ctl "$state" "$build" state | head -1)
  echo "   $st"
  grep -q "reclaim_timeout=1" <<<"$st" || { echo "timeout reclaim not counted" >&2; exit 1; }
  grep -q " opens=0 " <<<"$st" || { echo "state leaked after reclaims" >&2; exit 1; }

  echo "== [$label] ctl: state dump + forced reclaim"
  "$build/lnfs_accept_client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" "$data" 0 \
    >/dev/null 2>&1 || true  # leaves a holder with state behind (status irrelevant)
  dump=$(ctl "$state" "$build" state)
  cid=$(grep -m1 "states=1" <<<"$dump" | sed -E 's/^client (0x[0-9a-f]+).*/\1/' || true)
  if [[ -n $cid ]]; then
    out=$(ctl "$state" "$build" expire-client "$cid")
    grep -q reclaimed <<<"$out" || { echo "expire-client failed: $out" >&2; exit 1; }
    grep -q "reclaim_forced=1" <<<"$(ctl "$state" "$build" state | head -1)" || {
      echo "forced reclaim not counted" >&2; exit 1; }
  else
    echo "   (no client with open state to expire; skipped)"
  fi

  if [[ $label == rel ]]; then
    echo "== [$label] pynfs 4.1 write + session groups"
    "$repo/scripts/fetch_pynfs.sh" "$work/pynfs"
    (cd "$work/pynfs/nfs4.1" &&
     python3 ./testserver.py "127.0.0.1:$nfs_port$data" --minorversion=1 --noinit \
       --force open rename currentstateid verify courteous reclaim_complete \
       secinfo_no_name SEC1 SEC2 exchange_id create_session destroy_session \
       destroy_clientid sequence lookup lookupp putfh compound \
       noEID9 noEID50 > "$work/pynfs.log" 2>&1) || true
    tail -3 "$work/pynfs.log"
    grep -q "Of those" "$work/pynfs.log" || { echo "pynfs did not complete" >&2; exit 1; }
    # Expected failures on this milestone, by test code (scripts/pynfs_m5_expected.txt):
    # LOCK-dependent (phase 5), delegation / callback-dependent (M8), one pynfs-internal NameError (CSID7), and block/char
    # tree objects that need root to create.
    expected=$(tr '\n' ' ' < "$repo/scripts/pynfs_m5_expected.txt")
    unexpected=""
    for code in $(grep "FAILURE" "$work/pynfs.log" | awk '{print $1}' | sort -u); do
      grep -qw "$code" <<<"$expected" || unexpected="$unexpected $code"
    done
    if [[ -n $unexpected ]]; then
      echo "pynfs: unexpected failures:$unexpected" >&2
      grep -B1 -A2 FAILURE "$work/pynfs.log" >&2
      exit 1
    fi
  fi

  echo "== [$label] graceful shutdown + sanitizer check"
  # The daemon was re-spawned by the restart helper, so it is no longer our child:
  # poll for exit instead of wait(); the sanitizer verdict comes from its log.
  kill "$server_pid"
  local alive=1
  for _ in $(seq 1 200); do
    if ! kill -0 "$server_pid" 2>/dev/null; then alive=0; break; fi
    sleep 0.1
  done
  server_pid=""
  if [[ $alive -ne 0 ]] || grep -qE "ERROR: (Address|Leak)Sanitizer" "$log" ||
     ! grep -q "lightnfs stopped" "$log"; then
    echo "[$label] daemon did not stop cleanly or sanitizer report" >&2
    tail -40 "$log" >&2
    return 1
  fi
}

run_phase build-rel rel
run_phase build-asan asan

echo
echo "M6 (v4.2 sweets + v4.1 regression) loopback acceptance PASSED"
