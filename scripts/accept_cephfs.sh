#!/usr/bin/env bash
# CephFS backend acceptance against a real cluster (plan doc 10 §5.3 / design 06 §6.8
# — the design 09 "interface freeze" DoD needs a target environment; this is the
# runner for it).  No root on the gateway host: libcephfs is loaded at runtime, the
# export is served on loopback TCP, and the acceptance client (tests/accept_client.cpp)
# drives v3 + v4.1 + v4.2 workloads through it.  Byte verification needs the same
# filesystem mounted (kernel client or ceph-fuse) at a path that shows the export's
# files; without one the mount-dependent modes are skipped and the protocol-only modes
# still run.
#
#   LNFS_CEPH_CONF=/etc/ceph/ceph.conf LNFS_CEPH_ID=lightnfs \
#   [LNFS_CEPH_KEYRING=/etc/ceph/ceph.client.lightnfs.keyring] [LNFS_CEPH_FS=cephfs] \
#   [LNFS_CEPH_MON_HOST=mon1,mon2] [LNFS_CEPH_SUBDIR=/exports/lightnfs] \
#   [LNFS_CEPH_MOUNT=/mnt/cephfs/exports/lightnfs] scripts/accept_cephfs.sh [STRESS_SECONDS]
#
# Prerequisites on this host: libcephfs.so.2 (libcephfs2), a build directory (default
# build-rel, built here if missing), network reach to the monitors, MDS and OSDs, and a
# client key whose MDS caps cover the subdir with uid/gid pass-through (squash = "none":
# the client credentials are handed to libcephfs as the UserPerm of every call).  The
# subdir must exist in the filesystem.
set -euo pipefail

stress_secs=${1:-60}
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${LNFS_BUILD_DIR:-$repo/build-rel}
nfs_port=${LNFS_NFS_PORT:-12299}
mount_port=${LNFS_MOUNT_PORT:-12298}
conf=${LNFS_CEPH_CONF:-}
id=${LNFS_CEPH_ID:-}
keyring=${LNFS_CEPH_KEYRING:-}
fs_name=${LNFS_CEPH_FS:-}
mon_host=${LNFS_CEPH_MON_HOST:-}
subdir=${LNFS_CEPH_SUBDIR:-/}
mount=${LNFS_CEPH_MOUNT:-}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-cephfs.XXXXXX")
state="$work/state"
mkdir -p "$state"

server_pid=""
cleanup() {
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if ! ldconfig -p | grep -q libcephfs.so.2 && [[ -z ${LD_LIBRARY_PATH:-} ]]; then
  echo "libcephfs.so.2 not found (install libcephfs2, or set LD_LIBRARY_PATH)" >&2
  exit 2
fi

if [[ ! -x $build/lightnfsd ]]; then
  echo "== building $build (Release)"
  cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$build" -j"${LNFS_JOBS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}" \
    --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null
fi
"$repo/scripts/check_cephapi_abi.sh" || true

cat > "$work/lightnfs.toml" <<CFG
[server]
reactors = 0
offload_threads = 16
port = $nfs_port
mount_port = $mount_port
rpcbind = false
state_dir = "$state"
metrics_port = $((nfs_port + 1))
log_level = "info"

[protocol]
drc_ttl = "60s"
drc_mem = "16MiB"

[[export]]
path = "/cephfs"           # mount name only: the tree lives in the filesystem
backend = "cephfs"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.cephfs]
conf = "$conf"
id = "$id"
keyring = "$keyring"
mon_host = "$mon_host"
fs_name = "$fs_name"
subdir = "$subdir"
log_file = "$work/libcephfs.log"
CFG

echo "== config check"
"$build/lightnfsd" --check-config --config "$work/lightnfs.toml"

echo "== starting lightnfsd on 127.0.0.1:$nfs_port (fs '${fs_name:-<default>}' subdir $subdir)"
"$build/lightnfsd" --config "$work/lightnfs.toml" >"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 600); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "server exited during start:" >&2
    tail -30 "$work/server.log" >&2
    exit 1
  fi
  sleep 0.1
done
grep -q "cephfs export $subdir up" "$work/server.log" || { tail -30 "$work/server.log" >&2; exit 1; }
grep "backend traits" "$work/server.log"
# The whole point of this backend: both halves of multi-gateway coherence.
grep "backend traits" "$work/server.log" | grep -q "native-change=true" || {
  echo "!! native-change not reported: stx_version missing from the MDS reply?" >&2; exit 1; }
grep "backend traits" "$work/server.log" | grep -q "native-locks=true" || {
  echo "!! native-locks not reported" >&2; exit 1; }

client="$build/lnfs_accept_client"
export LIGHTNFS_CTL="$state/ctl.sock"

echo "== protocol-only modes (no backing path needed)"
"$client" connstorm 127.0.0.1 "$nfs_port" "$mount_port" 200 64
"$client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" /cephfs 90 || true
"$client" fsync-eio 127.0.0.1 "$nfs_port" "$mount_port" /cephfs || true

if [[ -n $mount && -d $mount ]]; then
  echo "== byte-verified modes against $mount"
  "$client" wtest  127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  "$client" walk   127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  "$client" v4walk 127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  "$client" v4rw   127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  "$client" v4lock 127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  "$client" v42    127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount"
  head -c 8388608 /dev/urandom > "$mount/soak.bin"
  "$client" stress 127.0.0.1 "$nfs_port" "$mount_port" /cephfs "$mount" soak.bin 8 32 "$stress_secs"
  rm -f "$mount/soak.bin"
else
  echo "== LNFS_CEPH_MOUNT not set: skipping byte-verified modes (wtest/walk/v4*/stress)"
fi

echo "== admin surface"
"$build/lightnfs-ctl" ping >/dev/null
"$build/lightnfs-ctl" fdcache | grep -q "backend=cephfs"
"$build/lightnfs-ctl" metrics | grep -q lightnfs_cephfs_objcache_hits_total
"$build/lightnfs-ctl" metrics | grep -q 'lightnfs_cephfs_blocklisted_total.* 0$'
"$build/lightnfs-ctl" state | grep -q .

cleanup
server_pid=""
if grep -qE "level=error" "$work/server.log"; then
  echo "!! errors in server log:" >&2
  grep -E "level=error" "$work/server.log" | head -20 >&2
  exit 1
fi
echo
echo "CephFS backend acceptance PASSED (fs '${fs_name:-<default>}' subdir $subdir)"
