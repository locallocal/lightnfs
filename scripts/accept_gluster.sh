#!/usr/bin/env bash
# GlusterFS backend acceptance against a real volume (plan doc 10 §5.3 — the design
# 09 "interface freeze" DoD needs a target environment; this is the runner for it).
# No root on the gateway host: libgfapi is loaded at runtime, the export is served on
# loopback TCP, and the acceptance client (tests/accept_client.cpp) drives v3 + v4.1 +
# v4.2 workloads through it.  Byte verification needs the same volume FUSE-mounted
# (or any path that shows the volume's files); without one the mounted-dependent
# modes are skipped and the protocol-only modes still run.
#
#   LNFS_GLUSTER_VOLUME=vol0 LNFS_GLUSTER_SERVERS=gs1,gs2:24007 \
#   [LNFS_GLUSTER_SUBDIR=/exports/lightnfs] [LNFS_GLUSTER_MOUNT=/mnt/vol0/exports/lightnfs] \
#   scripts/accept_gluster.sh [STRESS_SECONDS]
#
# Prerequisites on this host: libgfapi.so.0 (glusterfs-common / libgfapi0), a build
# directory (default build-rel, built here if missing), network reach to the volfile
# servers and bricks.  The subdir must exist in the volume and be writable for the
# gateway's identity (squash = "none": the client credentials are passed through).
set -euo pipefail

stress_secs=${1:-60}
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${LNFS_BUILD_DIR:-$repo/build-rel}
nfs_port=${LNFS_NFS_PORT:-12199}
mount_port=${LNFS_MOUNT_PORT:-12198}
volume=${LNFS_GLUSTER_VOLUME:?set LNFS_GLUSTER_VOLUME}
servers=${LNFS_GLUSTER_SERVERS:-localhost}
subdir=${LNFS_GLUSTER_SUBDIR:-/}
mount=${LNFS_GLUSTER_MOUNT:-}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-gluster.XXXXXX")
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

if ! ldconfig -p | grep -q libgfapi.so.0 && [[ -z ${LD_LIBRARY_PATH:-} ]]; then
  echo "libgfapi.so.0 not found (install glusterfs-common / libgfapi0, or set LD_LIBRARY_PATH)" >&2
  exit 2
fi

if [[ ! -x $build/lightnfsd ]]; then
  echo "== building $build (Release)"
  cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$build" --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null
fi
"$repo/scripts/check_gfapi_abi.sh" || true

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
path = "/gluster"          # mount name only: the tree lives in the volume
backend = "gluster"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.gluster]
volume = "$volume"
servers = "$servers"
subdir = "$subdir"
log_file = "$work/gfapi.log"
log_level = 7
CFG

echo "== config check"
"$build/lightnfsd" --check-config --config "$work/lightnfs.toml"

echo "== starting lightnfsd on 127.0.0.1:$nfs_port (volume $volume$subdir via $servers)"
"$build/lightnfsd" --config "$work/lightnfs.toml" >"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 300); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "server exited during start:" >&2
    tail -30 "$work/server.log" >&2
    exit 1
  fi
  sleep 0.1
done
grep -q "gluster volume $volume" "$work/server.log" || { tail -30 "$work/server.log" >&2; exit 1; }
grep "backend traits" "$work/server.log"

client="$build/lnfs_accept_client"
export LIGHTNFS_CTL="$state/ctl.sock"

echo "== protocol-only modes (no backing path needed)"
"$client" connstorm 127.0.0.1 "$nfs_port" "$mount_port" 200 64
"$client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" /gluster 90 || true
"$client" fsync-eio 127.0.0.1 "$nfs_port" "$mount_port" /gluster || true

if [[ -n $mount && -d $mount ]]; then
  echo "== byte-verified modes against $mount"
  "$client" wtest  127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  "$client" walk   127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  "$client" v4walk 127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  "$client" v4rw   127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  "$client" v4lock 127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  "$client" v42    127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount"
  head -c 8388608 /dev/urandom > "$mount/soak.bin"
  "$client" stress 127.0.0.1 "$nfs_port" "$mount_port" /gluster "$mount" soak.bin 8 32 "$stress_secs"
  rm -f "$mount/soak.bin"
else
  echo "== LNFS_GLUSTER_MOUNT not set: skipping byte-verified modes (wtest/walk/v4*/stress)"
fi

echo "== admin surface"
"$build/lightnfs-ctl" ping >/dev/null
"$build/lightnfs-ctl" fdcache | grep -q "backend=gluster"
"$build/lightnfs-ctl" metrics | grep -q lightnfs_gluster_objcache_hits_total
"$build/lightnfs-ctl" state | grep -q .

cleanup
server_pid=""
if grep -qE "level=error" "$work/server.log"; then
  echo "!! errors in server log:" >&2
  grep -E "level=error" "$work/server.log" | head -20 >&2
  exit 1
fi
echo
echo "GlusterFS backend acceptance PASSED (volume $volume$subdir)"
