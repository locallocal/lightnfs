#!/usr/bin/env bash
# Lustre backend acceptance against a real client mount (design 06 §6.5 — the design
# 09 "interface freeze" DoD needs a target environment; this is the runner for it).
# No root needed on the gateway host: filehandles are FIDs opened back through
# <mount>/.lustre/fid, the export is served on loopback TCP, and the acceptance client
# (tests/accept_client.cpp) drives v3 + v4.1 + v4.2 workloads through it.  Byte
# verification uses the same mount directly (it is a local path here, unlike gluster).
#
#   LNFS_LUSTRE_EXPORT=/mnt/lustre/lightnfs-accept [LNFS_LUSTRE_MOUNT=/mnt/lustre] \
#   [LNFS_LUSTRE_HSM=1] scripts/accept_lustre.sh [STRESS_SECONDS]
#
# Prerequisites: a Lustre client mount (ideally `-o flock` so native locks are
# MDS-arbitrated), an export directory inside it writable by the gateway user, a build
# directory (default build-rel, built here if missing).  With LNFS_LUSTRE_HSM=1 and an
# HSM coordinator + copytool running, the script additionally releases a file with
# `lfs hsm_release` and checks the gateway answers JUKEBOX/DELAY then serves it after
# the restore.
set -euo pipefail

stress_secs=${1:-60}
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${LNFS_BUILD_DIR:-$repo/build-rel}
nfs_port=${LNFS_NFS_PORT:-12299}
mount_port=${LNFS_MOUNT_PORT:-12298}
export_dir=${LNFS_LUSTRE_EXPORT:?set LNFS_LUSTRE_EXPORT to a directory inside the Lustre mount}
lustre_mount=${LNFS_LUSTRE_MOUNT:-}
want_hsm=${LNFS_LUSTRE_HSM:-0}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-lustre.XXXXXX")
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

[[ -d $export_dir ]] || { echo "$export_dir is not a directory" >&2; exit 2; }
fstype=$(stat -f -c %T "$export_dir")
if [[ $fstype != lustre ]]; then
  echo "$export_dir is on '$fstype', not lustre" >&2
  exit 2
fi

if [[ ! -x $build/lightnfsd ]]; then
  echo "== building $build (Release)"
  cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$build" --target lightnfsd lnfs_accept_client lightnfs-ctl >/dev/null
fi
"$repo/scripts/check_llapi_abi.sh" || true

mount_line=""
[[ -n $lustre_mount ]] && mount_line="mount = \"$lustre_mount\""
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
path = "$export_dir"
backend = "lustre"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false

[export.lustre]
$mount_line
hsm = true
native_locks = true
CFG

echo "== config check"
"$build/lightnfsd" --check-config --config "$work/lightnfs.toml"

echo "== starting lightnfsd on 127.0.0.1:$nfs_port (export $export_dir)"
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
grep -q "lustre export $export_dir: mount root" "$work/server.log" || { tail -30 "$work/server.log" >&2; exit 1; }
grep "backend traits" "$work/server.log"
grep -q "stable-handles=true" "$work/server.log"

client="$build/lnfs_accept_client"
export LIGHTNFS_CTL="$state/ctl.sock"

echo "== protocol-only modes"
"$client" connstorm 127.0.0.1 "$nfs_port" "$mount_port" 200 64
"$client" v4courtesy 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" 90 || true
"$client" fsync-eio 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" || true

echo "== byte-verified modes against $export_dir"
"$client" wtest  127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
"$client" walk   127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
"$client" v4walk 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
"$client" v4rw   127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
"$client" v4lock 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
"$client" v42    127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir"
head -c 8388608 /dev/urandom > "$export_dir/soak.bin"
"$client" stress 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir" soak.bin 8 32 "$stress_secs"
rm -f "$export_dir/soak.bin"

echo "== FID handle survives rename (resolve by FID, not path)"
echo fidtest > "$export_dir/fid-a"
fid_before=$(lfs path2fid "$export_dir/fid-a")
mv "$export_dir/fid-a" "$export_dir/fid-b"
[[ $(lfs path2fid "$export_dir/fid-b") == "$fid_before" ]]
rm -f "$export_dir/fid-b"

if [[ $want_hsm == 1 ]]; then
  echo "== HSM: released file → JUKEBOX/DELAY → restored"
  head -c 1048576 /dev/urandom > "$export_dir/cold.bin"
  lfs hsm_archive "$export_dir/cold.bin"
  for _ in $(seq 1 600); do lfs hsm_state "$export_dir/cold.bin" | grep -q archived && break; sleep 1; done
  lfs hsm_release "$export_dir/cold.bin"
  lfs hsm_state "$export_dir/cold.bin" | grep -q released
  before=$("$build/lightnfs-ctl" metrics | awk '/lightnfs_lustre_jukebox_total/{print $2}')
  # Readers hit the released file: the first data open kicks the restore and answers
  # JUKEBOX/DELAY (the client may report those as errors — only the counters matter).
  "$client" stress 127.0.0.1 "$nfs_port" "$mount_port" "$export_dir" "$export_dir" cold.bin 4 0 10 || true
  after=$("$build/lightnfs-ctl" metrics | awk '/lightnfs_lustre_jukebox_total/{print $2}')
  (( after > before )) || { echo "no JUKEBOX answered for the released file" >&2; exit 1; }
  "$build/lightnfs-ctl" metrics | grep -q 'lightnfs_lustre_hsm_restores_total[^ ]* [1-9]'
  for _ in $(seq 1 600); do lfs hsm_state "$export_dir/cold.bin" | grep -qv released && break; sleep 1; done
  rm -f "$export_dir/cold.bin"
fi

echo "== admin surface"
"$build/lightnfs-ctl" ping >/dev/null
"$build/lightnfs-ctl" fdcache | grep -q "export=$export_dir"
"$build/lightnfs-ctl" metrics | grep -q lightnfs_lustre_hsm_checks_total
"$build/lightnfs-ctl" metrics | grep -q lightnfs_lustre_lock_fds
"$build/lightnfs-ctl" state | grep -q .

cleanup
server_pid=""
if grep -qE "level=error" "$work/server.log"; then
  echo "!! errors in server log:" >&2
  grep -E "level=error" "$work/server.log" | head -20 >&2
  exit 1
fi
echo
echo "Lustre backend acceptance PASSED (export $export_dir)"
