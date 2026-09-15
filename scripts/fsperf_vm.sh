#!/usr/bin/env bash
# Metadata and data performance of a lightnfsd export, through a real kernel mount, on a
# root VM.  Starts a gateway over a local-backend export, measures the backing directory
# first, then mounts and measures the same thing at each requested NFS version — and
# prints the mount against the backing directory op by op, so the number you read is the
# cost of the NFS path rather than of the disk underneath.
#
#   fsperf_vm.sh [vers...]        # default: 3 4.1 4.2
#
# Needs root (mount) and the kernel NFS client.  Without root, run the tool directly
# against any directory — it is a plain filesystem benchmark:
#
#   scripts/fsperf.py /some/dir
#
# Results land in a directory printed at the end: one .json and one .txt per round, so a
# later run can --compare against them.
#
# env: LNFS_NFS_PORT (12099), LNFS_MOUNT_PORT (12098),
#      LNFS_FSPERF_ARGS (extra tool flags, default "--threads 4 --files 2000 --seconds 3"),
#      LNFS_FSPERF_OUT (result directory, default a fresh mktemp -d)
set -euo pipefail

versions=("$@")
[[ ${#versions[@]} -gt 0 ]] || versions=(3 4.1 4.2)
repo=$(cd "$(dirname "$0")/.." && pwd)
nfs_port=${LNFS_NFS_PORT:-12099}
mount_port=${LNFS_MOUNT_PORT:-12098}
tool_args=${LNFS_FSPERF_ARGS:---threads 4 --files 2000 --seconds 3}
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-fsperf.XXXXXX")
out=${LNFS_FSPERF_OUT:-$work/results}
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
cmake --build "$repo/build-rel" --target lightnfsd >/dev/null

mkdir -p "$data" "$work/state" "$mount_dir" "$out"
chmod 777 "$data"
cat > "$work/lightnfs.toml" <<EOF
[server]
reactors = 0
offload_threads = 16
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
fd_cache = 4096
EOF

"$repo/build-rel/lightnfsd" --config "$work/lightnfs.toml" >>"$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  (exec 3<>"/dev/tcp/127.0.0.1/$nfs_port") 2>/dev/null && break
  sleep 0.1
done

# The backing filesystem, as the reference: everything the mount does costs at least this.
echo
echo "== baseline: the backing directory"
# shellcheck disable=SC2086
python3 "$repo/scripts/fsperf.py" "$data" $tool_args --json "$out/backing.json" \
  | tee "$out/backing.txt"

for vers in "${versions[@]}"; do
  echo
  echo "== vers=$vers"
  opts="vers=$vers,tcp,port=${nfs_port},timeo=20,retrans=6"
  [[ $vers == 3 ]] && opts="$opts,mountport=${mount_port},nolock"
  sudo mount -t nfs -o "$opts" "127.0.0.1:${data}" "$mount_dir"
  echo "-- mount options in effect:"
  findmnt -no OPTIONS "$mount_dir" | tr ',' '\n' | sed 's/^/     /'
  tag="vers${vers//./_}"
  # --tolerance 1000 so the comparison never fails the run: this is NFS against local
  # disk, it is *expected* to be slower.  The percentages are what we came for.
  # shellcheck disable=SC2086
  python3 "$repo/scripts/fsperf.py" "$mount_dir" $tool_args \
    --json "$out/$tag.json" --compare "$out/backing.json" --tolerance 1000 \
    | tee "$out/$tag.txt"
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
echo "results in $out"
ls -1 "$out"
echo
echo "compare a later run against one of these:"
echo "  scripts/fsperf.py <dir> $tool_args --compare $out/vers${versions[0]//./_}.json"
