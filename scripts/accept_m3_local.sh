#!/usr/bin/env bash
# Phase-3 (M5) loopback acceptance — no root, no kernel mount (development plan §5.4,
# the locally-runnable half).  Against a real lightnfsd on loopback TCP:
#
#   unit       full ctest suite (Release + ASAN)
#   v4walk     vers=4.1 read path: EXCHANGE_ID/CREATE_SESSION, pseudo-root crossing,
#              recursive READDIR walk byte-verified, OPEN/READ/CLOSE, exactly-once slot
#              replay, minorversion-0 rejection
#   dual-read  the same tree walked via v3 (walk) and v4.1 (v4walk), both byte-verified
#              against the backing dir (v3/v4 read consistency anchor, §5.4)
#   pynfs      the 4.1 session groups (exchange_id/create_session/destroy_session/
#              destroy_clientid/reclaim_complete + sequence): write-dependent tests are
#              expected failures on the read-only milestone and are counted separately
#   asan       v4walk + v3 walk under ASAN, leak-checked graceful exit
#
# usage: accept_m3_local.sh
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12109}
mount_port=${LNFS_MOUNT_PORT:-12108}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m3.XXXXXX")

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building Release and ASAN configurations"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lnfs_accept_client lnfs_tests >/dev/null
cmake -S "$repo" -B "$repo/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLNFS_SANITIZE=address >/dev/null
cmake --build "$repo/build-asan" --target lightnfsd lnfs_accept_client lnfs_tests >/dev/null

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
# pynfs object tree ({path}/tree/<type>); block/char need root and are staged only
# when available (the VM acceptance covers them).
echo data > "$data/tree/file"
ln -sf file "$data/tree/link"
mkfifo "$data/tree/fifo" 2>/dev/null || true
python3 - "$data/tree/socket" <<'PYEOF' 2>/dev/null || true
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
PYEOF
sudo mknod "$data/tree/block" b 7 99 2>/dev/null || true
sudo mknod "$data/tree/char" c 1 3 2>/dev/null || true

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
path = "$data"
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
  tail -20 "$2" >&2
  return 1
}

run_phase() {  # $1 build dir, $2 label
  local build="$repo/$1" label=$2
  local state="/tmp/lnfs-m3-state-$label" log="$work/server-$label.log"
  rm -rf "$state"
  mkdir -p "$state"
  write_config "$state"
  ASAN_OPTIONS="detect_leaks=1:exitcode=42" \
    "$build/lightnfsd" --config "$work/lightnfs.toml" >"$log" 2>&1 &
  server_pid=$!
  wait_port "$nfs_port" "$log"

  echo "== [$label] v4walk (vers=4.1 sessions, byte-verified)"
  "$build/lnfs_accept_client" v4walk 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
  echo "== [$label] v3 walk (dual-read consistency)"
  "$build/lnfs_accept_client" walk 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"

  if [[ $label == rel ]]; then
    echo "== [$label] pynfs 4.1 session groups"
    "$repo/scripts/fetch_pynfs.sh" "$work/pynfs"
    (cd "$work/pynfs/nfs4.1" &&
     python3 ./testserver.py "127.0.0.1:$nfs_port$data" --minorversion=1 --noinit \
       --force exchange_id create_session destroy_session destroy_clientid \
       reclaim_complete sequence lookup lookupp putfh compound secinfo_no_name \
       noEID9 noEID50 > "$work/pynfs.log" 2>&1) || true
    tail -3 "$work/pynfs.log"
    grep -q "Of those" "$work/pynfs.log" || { echo "pynfs did not complete" >&2; exit 1; }
    # Expected failures on this milestone, by test code:
    #  - write-dependent tests (phase 4): DELEG5-7, SEQ9b/9d/10b, RECC2/3,
    #    DSESS1/9002/9003
    #  - block/char tree objects, absent without root: PUTFH1b/1c, LKPP1b/1c
    expected="DELEG5 DELEG6 DELEG7 SEQ9b SEQ9d SEQ10b RECC2 RECC3 DSESS1 DSESS9002 \
DSESS9003 PUTFH1b PUTFH1c LKPP1b LKPP1c"
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
  kill "$server_pid"
  local rc=0
  wait "$server_pid" || rc=$?
  server_pid=""
  if [[ $rc -ne 0 ]] || grep -qE "ERROR: (Address|Leak)Sanitizer" "$log"; then
    echo "[$label] exit rc=$rc or sanitizer report" >&2
    tail -40 "$log" >&2
    return 1
  fi
}

run_phase build-rel rel
run_phase build-asan asan

echo
echo "M3 (v4.1 read-only) loopback acceptance PASSED"
