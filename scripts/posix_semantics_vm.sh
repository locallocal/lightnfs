#!/usr/bin/env bash
# POSIX semantics of a lightnfsd export, through a real kernel mount, on a root VM.
# Starts a gateway over a local-backend export, mounts it at each requested NFS version
# and runs scripts/posix_semantics.py against the mountpoint; then runs the same checker
# against the backing directory, so a difference is attributable to the NFS path rather
# than to the filesystem underneath.
#
#   posix_semantics_vm.sh [vers...]      # default: 3 4.1 4.2
#
# Needs root (mount) and the kernel NFS client.  Without root, run the checker directly
# against any directory — it is a plain filesystem checker:
#
#   scripts/posix_semantics.py /some/dir
#
# Expected differences by version (the script encodes them, they are not failures):
#   vers=3   no byte-range locks — lightnfs implements no NLM/NSM (design 04, D8), so the
#            mount is made with -o nolock and the `lock` group is skipped.
#   vers=4.1 full byte-range locks; everything runs.
#   vers=4.2 as 4.1, plus the sparse group is meaningful (SEEK_HOLE/SEEK_DATA reach the
#            server as SEEK); under 4.1 the client answers them locally.
#
# env: LNFS_NFS_PORT (12099), LNFS_MOUNT_PORT (12098), LNFS_POSIX_ARGS (extra checker
#      flags, e.g. "-v" or "--skip times")
set -euo pipefail

# Build parallelism: the repo convention (ci.sh, coverage.sh, fuzz.sh) — half the cores
# unless told otherwise.  Ninja defaults to all of them, which an ASAN build does not fit
# into on a modest box.
jobs=${LNFS_JOBS:-$(($(nproc) / 2))}

versions=("$@")
[[ ${#versions[@]} -gt 0 ]] || versions=(3 4.1 4.2)
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12099}
mount_port=${LNFS_MOUNT_PORT:-12098}
extra=${LNFS_POSIX_ARGS:-}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-posix.XXXXXX")
data="$work/data"
mount_dir="$work/mnt"

server_pid=""
cleanup() {
  if mountpoint -q "$mount_dir" 2>/dev/null; then sudo umount -f "$mount_dir" || true; fi
  if [[ -n $server_pid ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== building lightnfsd (Release)"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" -j"$jobs" --target lightnfsd >/dev/null

mkdir -p "$data" "$work/state" "$mount_dir"
chmod 777 "$data"
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
readonly = false

[export.local]
handles = "auto"
EOF

"$repo/build-rel/lightnfsd" --config "$work/lightnfs.toml" >>"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  sleep 0.1
done

# The backing filesystem itself, as the control: whatever it fails, the mount may fail
# too without that being lightnfsd's doing.
echo
echo "== baseline: the backing directory ($(stat -f -c %T "$data"))"
baseline_rc=0
python3 "$repo/scripts/posix_semantics.py" "$data" $extra || baseline_rc=$?
[[ $baseline_rc -eq 0 ]] || echo "!! the backing filesystem itself is not clean — compare below"

rc=0
for vers in "${versions[@]}"; do
  echo
  echo "== vers=$vers"
  opts="vers=$vers,tcp,port=${nfs_port},timeo=20,retrans=6"
  skip=""
  if [[ $vers == 3 ]]; then
    # No NLM/NSM: the kernel would block forever trying to lock, so mount -o nolock and
    # let the checker skip the group rather than hang.
    opts="$opts,mountport=${mount_port},nolock"
    skip="--skip lock"
  fi
  sudo mount -t nfs -o "$opts" "127.0.0.1:${data}" "$mount_dir"
  # shellcheck disable=SC2086
  python3 "$repo/scripts/posix_semantics.py" "$mount_dir" $skip $extra || rc=$?
  sudo umount "$mount_dir"
done

kill "$server_pid"
wait "$server_pid" || { echo "server exited non-zero (see $work/server.log)" >&2; exit 1; }
server_pid=""

if grep -q 'level=error' "$work/server.log"; then
  echo "!! level=error lines in the server log:" >&2
  grep 'level=error' "$work/server.log" >&2
  exit 1
fi

echo
if [[ $rc -eq 0 ]]; then
  echo "POSIX semantics PASSED for vers: ${versions[*]}"
else
  echo "POSIX semantics FAILED for at least one version (see the FAIL lines above)" >&2
fi
exit $rc
