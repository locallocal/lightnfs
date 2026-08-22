#!/usr/bin/env bash
# Phase-5 (M7) kernel-mount acceptance on a root VM (development plan §6.3): real
# `mount -o vers=4.1` read-write, cthon04 basic/general/special, fsx, a kill -9
# restart with open state held (the 07 §7.5 reclaim scenario: the kernel client
# reclaims inside grace and data is intact), a lease-expiry run (userspace holder
# vanishes → conflict and timeout reclaims counted via lightnfs-ctl), and a v3/v4
# mixed-write pass over the same export (documented boundary: no cross-version lock
# semantics; each side's writes land and are visible to the other).
#
# Adds over M4: cthon lock group (-l), kernel POSIX byte-range locking (fcntl), and the
# userspace v4lock scenario.
#
# usage: accept_m5_vm.sh [FSX_OPS]   (overnight: FSX_OPS=2000000)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
fsx_ops=${1:-50000}
nfs_port=${LNFS_NFS_PORT:-12119}
mount_port=${LNFS_MOUNT_PORT:-12118}
lease=${LNFS_LEASE:-10}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m5vm.XXXXXX")
data="$work/data"
mnt4="$work/mnt4"
mnt3="$work/mnt3"
state="$work/state"

server_pid=""
cleanup() {
  for m in "$mnt4" "$mnt3"; do
    if mountpoint -q "$m" 2>/dev/null; then sudo umount -f "$m" || true; fi
  done
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building lightnfsd (Release) + cthon04 + fsx"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null
"$repo/scripts/fetch_cthon.sh" "$work/cthon04"
make -C "$work/cthon04" 2>/dev/null >/dev/null || make -C "$work/cthon04" >/dev/null
"$repo/scripts/fetch_fsx.sh" "$work/fsxdir"

mkdir -p "$data" "$state" "$mnt4" "$mnt3"
cat > "$work/lightnfs.toml" <<EOC
[server]
reactors = 0
offload_threads = 8
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
ctl() { LIGHTNFS_CTL="$state/ctl.sock" "$repo/build-rel/lightnfs-ctl" "$@"; }

echo "== starting lightnfsd + mount -o vers=4.1 (rw)"
start_server
sudo mount -t nfs -o "vers=4.1,port=${nfs_port},timeo=20,retrans=6" \
  "127.0.0.1:${data}" "$mnt4"
sudo chmod 777 "$mnt4"

echo "== cthon04 basic/general/special/lock (vers=4.1)"
export NFSTESTDIR="$mnt4/cthon-test"
for suite in -b -g -s -l; do
  echo "---- cthon $suite"
  rm -rf "$NFSTESTDIR"
  (cd "$work/cthon04" && ./runtests "$suite" -f "$NFSTESTDIR")
done

echo "== fsx ($fsx_ops ops, vers=4.1)"
"$work/fsxdir/fsx" -N "$fsx_ops" -p 10000 "$mnt4/fsx.dat"
rm -f "$mnt4/fsx.dat"

echo "== kill -9 + restart with open state held: kernel client reclaims inside grace"
# A writer keeps the file open across the restart; its later writes must land
# (CLAIM_PREVIOUS reclaim) and nothing written before the crash may be lost.
python3 - "$mnt4/held.bin" "$work/held.ready" "$work/held.go" >"$work/held.log" 2>&1 <<'PY' &
import os, sys, time
path, ready, go = sys.argv[1:4]
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
os.write(fd, b"before-crash\n"); os.fsync(fd)
open(ready, "w").close()
while not os.path.exists(go): time.sleep(0.1)
os.write(fd, b"after-restart\n"); os.fsync(fd)
os.close(fd)
PY
holder=$!
while [[ ! -e $work/held.ready ]]; do sleep 0.1; done
kill -9 "$server_pid"
server_pid=""
sleep 0.5
start_server
grep -q "grace period armed" "$work/server.log" || { echo "server did not enter grace" >&2; exit 1; }
touch "$work/held.go"
wait "$holder"
cmp <(printf 'before-crash\nafter-restart\n') "$data/held.bin"
ls "$mnt4" >/dev/null
echo "   reclaimed: $(ctl state | head -1)"
sleep "$((lease + 2))"   # let grace end on its own if the client did not send RECLAIM_COMPLETE
if ! grep -q "leaving grace early" "$work/server.log"; then
  echo "   (grace ended by timeout)"
fi

echo "== byte-range locks over the kernel mount (flock + fcntl POSIX ranges)"
python3 - "$mnt4/locktest.bin" <<'PY'
import fcntl, os, struct, sys
path = sys.argv[1]
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644); os.write(fd, b"x" * 4096)
# POSIX write lock on [0,1024): F_SETLK must succeed, F_GETLK from a 2nd fd must see it.
fcntl.lockf(fd, fcntl.LOCK_EX, 1024, 0, os.SEEK_SET)
fd2 = os.open(path, os.O_RDWR)
try:
    fcntl.lockf(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB, 512, 0, os.SEEK_SET)
    print("FAIL: conflicting lock unexpectedly granted"); sys.exit(1)
except OSError:
    pass
fcntl.lockf(fd, fcntl.LOCK_UN, 1024, 0, os.SEEK_SET)
fcntl.lockf(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB, 512, 0, os.SEEK_SET)  # now free
os.close(fd2); os.close(fd)
print("kernel byte-range locking over vers=4.1 OK")
PY
rm -f "$mnt4/locktest.bin"

echo "== lease expiry: courtesy conflict + timeout reclaims (userspace holders)"
"$repo/build-rel/lnfs_accept_client" v4lock 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"
"$repo/build-rel/lnfs_accept_client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$lease"
sleep "$((2 * lease + 3))"
st=$(ctl state | head -1)
echo "   $st"
grep -q "reclaim_conflict=1" <<<"$st" || { echo "conflict reclaim not counted" >&2; exit 1; }
grep -q "reclaim_timeout=1" <<<"$st" || { echo "timeout reclaim not counted" >&2; exit 1; }

echo "== v3/v4 mixed writes on the same export (documented boundary behaviour)"
sudo mount -t nfs -o "vers=3,tcp,nolock,port=${nfs_port},mountport=${mount_port}" \
  "127.0.0.1:${data}" "$mnt3"
sudo chmod 777 "$mnt3"
for i in $(seq 1 20); do
  (head -c 65536 /dev/urandom > "$mnt3/mixed-v3-$i.bin") &
  (head -c 65536 /dev/urandom > "$mnt4/mixed-v4-$i.bin") &
done
wait
for i in $(seq 1 20); do
  cmp "$mnt3/mixed-v3-$i.bin" "$mnt4/mixed-v3-$i.bin"   # v3 write visible via v4
  cmp "$mnt4/mixed-v4-$i.bin" "$mnt3/mixed-v4-$i.bin"   # v4 write visible via v3
done
# A v4 deny-WRITE open does not stop a v3 writer (v3 has no share semantics): the
# boundary is observable, not silent.
rm -f "$mnt3"/mixed-*.bin

sudo umount "$mnt3"
sudo umount "$mnt4"
kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero" >&2; tail -20 "$work/server.log" >&2; exit 1; }
server_pid=""
echo
echo "M4 VM mount acceptance PASSED (vers=4.1 rw: cthon b/g/s/l + fsx $fsx_ops + kernel locks + restart reclaim + lease reclaim + v3/v4 mixed)"
