#!/usr/bin/env bash
# Regenerates the syscall allowlist for the systemd unit's seccomp filter by running a
# real server through strace during a full v3 + v4.1 (read/write/lock) workload
# (security checklist §8.5 item 7).  Prints the sorted syscall set; compare it against
# packaging/systemd/lightnfs.service after any runtime change.
#
# usage: gen_seccomp_allowlist.sh [BUILD_DIR]
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$repo/build-rel}
command -v strace >/dev/null || { echo "strace not installed" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-seccomp.XXXXXX")
port=${LNFS_NFS_PORT:-12139}
mount_port=${LNFS_MOUNT_PORT:-12138}
mkdir -p "$work/data/sub" "$work/state"
echo hi > "$work/data/hello.txt"
head -c 100000 /dev/urandom > "$work/data/sub/x.bin"
cat > "$work/lightnfs.toml" <<EOC
[server]
reactors = 0
offload_threads = 4
port = $port
mount_port = $mount_port
rpcbind = false
state_dir = "$work/state"
[[export]]
path = "$work/data"
backend = "local"
fsid = 1
clients = ["127.0.0.0/8"]
squash = "none"
readonly = false
[export.local]
handles = "auto"
EOC
strace -f -qq -e trace=all -o "$work/strace.log" \
  "$build/lightnfsd" --config "$work/lightnfs.toml" >"$work/server.log" 2>&1 &
srv=$!
for _ in $(seq 1 100); do (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break; sleep 0.1; done
"$build/lnfs_accept_client" v4rw   127.0.0.1 "$port" "$mount_port" "$work/data" "$work/data" >/dev/null
"$build/lnfs_accept_client" v4walk 127.0.0.1 "$port" "$mount_port" "$work/data" "$work/data" >/dev/null
"$build/lnfs_accept_client" walk   127.0.0.1 "$port" "$mount_port" "$work/data" "$work/data" >/dev/null
kill -INT "$srv" 2>/dev/null || true
wait "$srv" 2>/dev/null || true
echo "syscalls issued by lightnfsd during a full v3+v4.1 rw+lock run:"
grep -oE "^[0-9]+ +[a-z_0-9]+\(" "$work/strace.log" | awk '{print $2}' | tr -d '(' | sort -u | tr '\n' ' '
echo
rm -rf "$work"
