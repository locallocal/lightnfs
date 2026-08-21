#!/usr/bin/env bash
# Phase-3 kernel-mount acceptance on a root VM (development plan §5.4): real
# `mount -o vers=4.1,ro` browse + read, plus a v3/v4 dual-mount concurrent-read
# comparison over the same export (both mounts diffed against each other and the
# backing tree).  Reused by CI (the runner is a root VM).
#
# usage: accept_m3_vm.sh
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12109}
mount_port=${LNFS_MOUNT_PORT:-12108}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-m3vm.XXXXXX")
data="$work/data"
mnt3="$work/mnt3"
mnt4="$work/mnt4"

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

echo "== building lightnfsd (Release)"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd >/dev/null

echo "== dataset"
mkdir -p "$data/dir1/dir2" "$mnt3" "$mnt4" "$work/state"
for i in $(seq 1 50); do head -c $((i * 1000)) /dev/urandom > "$data/f$i.bin"; done
head -c 5000000 /dev/urandom > "$data/dir1/big.bin"
ln -s f1.bin "$data/link"
(cd "$data" && find . -type f -print0 | sort -z | xargs -0 md5sum > "$work/manifest.md5")

cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 8
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$work/state"

[[export]]
path = "$data"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
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

echo "== mount vers=4.1,ro (via pseudo-root server:/)"
sudo mount -t nfs -o "vers=4.1,ro,port=${nfs_port},timeo=20,retrans=6" \
  "127.0.0.1:${data}" "$mnt4"
echo "== mount vers=3,ro (same export)"
sudo mount -t nfs -o "vers=3,tcp,ro,nolock,port=${nfs_port},mountport=${mount_port}" \
  "127.0.0.1:${data}" "$mnt3"

echo "== v4.1: ls -lR + cat + md5sum -c"
ls -lR "$mnt4" >/dev/null
find "$mnt4" -type f -print0 | xargs -0 -r cat >/dev/null
(cd "$mnt4" && md5sum --quiet -c "$work/manifest.md5")

echo "== v3/v4 dual-mount concurrent read comparison"
(find "$mnt3" -type f -print0 | xargs -0 -r cat >/dev/null) &
reader3=$!
(find "$mnt4" -type f -print0 | xargs -0 -r cat >/dev/null) &
reader4=$!
wait $reader3 $reader4
diff -r "$mnt3" "$mnt4"
(cd "$mnt3" && md5sum --quiet -c "$work/manifest.md5")

echo "== write attempts on ro v4.1 mount must fail"
if sudo touch "$mnt4/nope" 2>/dev/null; then
  echo "write on ro v4.1 mount unexpectedly succeeded" >&2
  exit 1
fi

sudo umount "$mnt4"
sudo umount "$mnt3"
kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero" >&2; tail -20 "$work/server.log" >&2; exit 1; }
server_pid=""
echo
echo "M3 VM mount acceptance PASSED (vers=4.1,ro + v3/v4 dual-mount consistency)"
