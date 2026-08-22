#!/usr/bin/env bash
# Phase-6 (M8 item 1) kernel-mount acceptance on a root VM (development plan §8): a real
# `mount -o vers=4.2`, then the Linux client's v4.2 paths exercised through ordinary
# tools and verified against the backing tree:
#
#   sparse    fallocate -p (DEALLOCATE), truncate + fallocate -l (ALLOCATE), SEEK_HOLE/
#             SEEK_DATA from python (SEEK) — hole map compared between mount and backing
#   copy      cp of a 64 MiB file (client issues COPY via copy_file_range), byte-verified,
#             plus a partial copy_file_range at an offset
#   clone     cp --reflink=always: succeeds on XFS(reflink)/Btrfs exports, otherwise the
#             client falls back (NOTSUPP) and --reflink=auto still copies
#   regress   cthon basic/general at vers=4.2 (the 4.1 machinery is unchanged under 4.2)
#   userspace the v42 loopback scenario against the same server
#
# usage: accept_m6_vm.sh
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12119}
mount_port=${LNFS_MOUNT_PORT:-12118}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m6vm.XXXXXX")
data="$work/data"
mnt="$work/mnt42"
state="$work/state"

server_pid=""
cleanup() {
  if mountpoint -q "$mnt" 2>/dev/null; then sudo umount -f "$mnt" || true; fi
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -9 "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building lightnfsd (Release) + cthon04"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null
"$repo/scripts/fetch_cthon.sh" "$work/cthon04"
make -C "$work/cthon04" 2>/dev/null >/dev/null || make -C "$work/cthon04" >/dev/null

mkdir -p "$data" "$state" "$mnt"
cat > "$work/lightnfs.toml" <<EOC
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$state"

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

"$repo/build-rel/lightnfsd" --config "$work/lightnfs.toml" >>"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  sleep 0.1
done
caps=$(grep -m1 "v4.2 capabilities" "$work/server.log" || true)
echo "   $caps"
clone_cap=$(grep -o "clone=[a-z]*" <<<"$caps" | cut -d= -f2)

echo "== mount -o vers=4.2"
sudo mount -t nfs -o "vers=4.2,port=${nfs_port},timeo=20,retrans=6" "127.0.0.1:${data}" "$mnt"
sudo chmod 777 "$mnt"
grep -q "vers=4.2" /proc/mounts || { echo "mount did not negotiate 4.2" >&2; exit 1; }

echo "== sparse: DEALLOCATE / ALLOCATE / SEEK through fallocate(1), truncate(1), lseek(2)"
head -c $((3 << 20)) /dev/urandom > "$mnt/sparse.bin"
fallocate -p -o $((1 << 20)) -l $((1 << 20)) "$mnt/sparse.bin"       # DEALLOCATE
[[ $(stat -c %s "$mnt/sparse.bin") -eq $((3 << 20)) ]] || { echo "punch changed size" >&2; exit 1; }
cmp <(dd if="$mnt/sparse.bin" bs=1M skip=1 count=1 2>/dev/null) <(head -c $((1 << 20)) /dev/zero)
fallocate -o $((3 << 20)) -l $((1 << 20)) "$mnt/sparse.bin"          # ALLOCATE (extends)
[[ $(stat -c %s "$mnt/sparse.bin") -eq $((4 << 20)) ]] || { echo "allocate did not extend" >&2; exit 1; }
python3 - "$mnt/sparse.bin" "$data/sparse.bin" <<'PY'
import os, sys
def holes(path):
    fd = os.open(path, os.O_RDONLY); size = os.fstat(fd).st_size; out = []; off = 0
    while off < size:
        try: h = os.lseek(fd, off, os.SEEK_HOLE)
        except OSError: break
        if h >= size: break
        try: d = os.lseek(fd, h, os.SEEK_DATA)
        except OSError: d = size
        out.append((h, d)); off = d
    os.close(fd); return out
a, b = holes(sys.argv[1]), holes(sys.argv[2])
print("   holes via mount:", a); print("   holes on backing:", b)
assert a == b, "SEEK_HOLE/SEEK_DATA map differs between vers=4.2 mount and backing file"
assert any(h <= (1 << 20) and d >= (2 << 20) for h, d in a), "punched hole not visible via SEEK"
PY
sync

echo "== COPY: cp of a 64 MiB file + copy_file_range at an offset"
head -c $((64 << 20)) /dev/urandom > "$mnt/copy-src.bin"
sync
cp "$mnt/copy-src.bin" "$mnt/copy-dst.bin"
cmp "$data/copy-src.bin" "$data/copy-dst.bin"
python3 - "$mnt/copy-src.bin" "$mnt/copy-part.bin" <<'PY'
import os, sys
s = os.open(sys.argv[1], os.O_RDONLY); d = os.open(sys.argv[2], os.O_RDWR | os.O_CREAT, 0o644)
n = os.copy_file_range(s, d, 1 << 20, 4 << 20, 2 << 20)
assert n == (1 << 20), n
os.fsync(d); os.close(s); os.close(d)
print("   copy_file_range(offset 4M -> 2M, 1M) OK")
PY
cmp <(dd if="$data/copy-src.bin" bs=1M skip=4 count=1 2>/dev/null) \
    <(dd if="$data/copy-part.bin" bs=1M skip=2 count=1 2>/dev/null)

echo "== CLONE: cp --reflink (export clone capability: ${clone_cap:-unknown})"
if [[ $clone_cap == true ]]; then
  cp --reflink=always "$mnt/copy-src.bin" "$mnt/clone.bin"
  cmp "$data/copy-src.bin" "$data/clone.bin"
  echo "   reflink clone verified"
else
  if cp --reflink=always "$mnt/copy-src.bin" "$mnt/clone.bin" 2>/dev/null; then
    echo "cp --reflink=always succeeded without clone capability" >&2; exit 1
  fi
  cp --reflink=auto "$mnt/copy-src.bin" "$mnt/clone.bin"
  cmp "$data/copy-src.bin" "$data/clone.bin"
  echo "   reflink refused (NOTSUPP), --reflink=auto fell back to COPY"
fi

echo "== cthon04 basic/general at vers=4.2 (regression of the 4.1 machinery)"
export NFSTESTDIR="$mnt/cthon-test"
for suite in -b -g; do
  echo "---- cthon $suite"
  rm -rf "$NFSTESTDIR"
  (cd "$work/cthon04" && ./runtests "$suite" -f "$NFSTESTDIR")
done

echo "== userspace v42 scenario against the same server"
"$repo/build-rel/lnfs_accept_client" v42 127.0.0.1 "$nfs_port" "$mount_port" "$data" "$data"

sudo umount "$mnt"
kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero" >&2; tail -20 "$work/server.log" >&2; exit 1; }
server_pid=""
echo
echo "M6 VM mount acceptance PASSED (vers=4.2: sparse + COPY + CLONE/fallback + cthon b/g + v42)"
