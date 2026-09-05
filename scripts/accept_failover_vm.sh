#!/usr/bin/env bash
# Multi-gateway failover on a root VM with a real kernel NFSv4.1 mount (design 09,
# plan 10 E1/E2) — the half that needs root and a real client, so it is NOT in CI and
# is triggered by hand.  Two lightnfsd share one cluster shared_dir and one export
# tree; a real `mount -t nfs -o vers=4.1` holds an open + POSIX lock + unstable write,
# gateway A is killed, gateway B takes the fence, and the same mount reclaims its state
# once it reconnects (through the VIP in a keepalived setup, or a manual remount here).
#
# Backends (one round each, selected by $1; default: local):
#   local    two gateways over one shared directory on a btime filesystem (no cluster
#            needed — runnable on any root VM; unsafe_skip_backend_checks)
#   gluster  set LNFS_GLUSTER_VOLUME / LNFS_GLUSTER_SERVERS; the volume is the shared FS
#   cephfs   set LNFS_CEPHFS_* (mon_host/fs_name/keyring); [export.cephfs] uuid drives D2
#   lustre   set LNFS_LUSTRE_MOUNT to a client mount; native_locks over OFD
# For gluster/lustre/cephfs the export tree and shared_dir must live on the cluster FS
# so both gateways see one namespace; those rounds need the cluster reachable.
#
# Without keepalived the client cannot follow the failover transparently: this script
# points the mount at $LNFS_VIP (default 127.0.0.1) and, when that is loopback,
# remounts against gateway B after the takeover.  On a real VIP the same mount survives.
#
# usage: sudo LNFS_VIP=10.0.0.9 accept_failover_vm.sh [local|gluster|cephfs|lustre]
set -euo pipefail

backend=${1:-local}
repo=$(cd "$(dirname "$0")/.." && pwd)
vip=${LNFS_VIP:-127.0.0.1}
port_a=${LNFS_PORT_A:-12219}; mount_a=${LNFS_MOUNT_A:-12218}
port_b=${LNFS_PORT_B:-12229}; mount_b=${LNFS_MOUNT_B:-12228}
fence_lease_ms=${LNFS_FENCE_LEASE_MS:-2000}
lease=${LNFS_LEASE:-10}

[[ $EUID -eq 0 ]] || { echo "must run as root (kernel mount)"; exit 2; }
command -v mount.nfs >/dev/null || { echo "mount.nfs (nfs-common) required"; exit 2; }

work=$(mktemp -d "${TMPDIR:-/var/tmp}/lnfs-fovm.XXXXXX")   # /var/tmp: ext4, has btime
data="$work/data"; shared="$work/shared"; mnt="$work/mnt"
pid_a=""; pid_b=""
cleanup() {
  mountpoint -q "$mnt" 2>/dev/null && umount -f "$mnt" 2>/dev/null || true
  for p in $pid_a $pid_b; do kill -9 "$p" 2>/dev/null || true; done
  [[ ${LNFS_KEEP_WORK:-0} = 1 ]] || rm -rf "$work"
}
trap cleanup EXIT

echo "== building lightnfsd (Release)"
cmake -S "$repo" -B "$repo/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$repo/build-rel" --target lightnfsd lightnfs-ctl >/dev/null
bin="$repo/build-rel"
mkdir -p "$data" "$shared" "$mnt" "$work/a" "$work/b"

# Per-backend export sub-table and the safety knob for the local round.
export_block() {
  case $backend in
    local)
      printf '[[export]]\npath = "%s"\nbackend = "local"\nfsid = 1\nclients = ["0.0.0.0/0"]\nsquash = "none"\n[export.local]\nhandles = "auto"\n' "$data" ;;
    gluster)
      printf '[[export]]\npath = "/export/data"\nbackend = "gluster"\nfsid = 1\nclients = ["0.0.0.0/0"]\nsquash = "none"\n[export.gluster]\nvolume = "%s"\nservers = "%s"\n' \
        "${LNFS_GLUSTER_VOLUME:?set LNFS_GLUSTER_VOLUME}" "${LNFS_GLUSTER_SERVERS:?set LNFS_GLUSTER_SERVERS}" ;;
    cephfs)
      printf '[[export]]\npath = "/export/data"\nbackend = "cephfs"\nfsid = 1\nclients = ["0.0.0.0/0"]\nsquash = "none"\n[export.cephfs]\nmon_host = "%s"\nfs_name = "%s"\nkeyring = "%s"\nuuid = "failover-vm-1"\n' \
        "${LNFS_CEPHFS_MON:?set LNFS_CEPHFS_MON}" "${LNFS_CEPHFS_FS:?set LNFS_CEPHFS_FS}" "${LNFS_CEPHFS_KEYRING:?set LNFS_CEPHFS_KEYRING}" ;;
    lustre)
      printf '[[export]]\npath = "%s/export"\nbackend = "lustre"\nfsid = 1\nclients = ["0.0.0.0/0"]\nsquash = "none"\n[export.lustre]\nmount = "%s"\n' \
        "${LNFS_LUSTRE_MOUNT:?set LNFS_LUSTRE_MOUNT}" "${LNFS_LUSTRE_MOUNT:?}" ;;
    *) echo "unknown backend: $backend" >&2; exit 2 ;;
  esac
  [[ $backend == local ]] && echo "# local shared_dir stands in for the cluster FS"
}

write_config() {  # $1 node, $2 state, $3 nfs port, $4 mount port, $5 role
  { cat <<EOC
[server]
reactors = 0
offload_threads = 8
port = $3
mount_port = $4
rpcbind = false
state_dir = "$2"
bind = "0.0.0.0"

[protocol]
lease = "${lease}s"

[cluster]
enabled = true
id = "failover-vm-cluster"
shared_dir = "$shared"
node = "gw-$1"
role = "$5"
takeover = "manual"
fence_lease = "${fence_lease_ms}ms"
EOC
    [[ $backend == local ]] && echo "unsafe_skip_backend_checks = true"
    echo
    export_block
  } > "$work/$1.toml"
}

wait_port() { for _ in $(seq 1 100); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.1; done; return 1; }
cstat() { LIGHTNFS_CTL="$work/$1/ctl.sock" "$bin/lightnfs-ctl" cluster "${@:2}"; }

write_config a "$work/a" "$port_a" "$mount_a" active
write_config b "$work/b" "$port_b" "$mount_b" standby
"$bin/lightnfsd" --config "$work/a.toml" >>"$work/a.log" 2>&1 & pid_a=$!
"$bin/lightnfsd" --config "$work/b.toml" >>"$work/b.log" 2>&1 & pid_b=$!
wait_port "$port_a"; for _ in $(seq 1 50); do [[ -S "$work/b/ctl.sock" ]] && break; sleep 0.1; done
echo "== A: $(cstat a status)"
echo "== B: $(cstat b status)"

echo "== kernel mount vers=4.1 via $vip:$port_a and hold state"
mount -t nfs -o "vers=4.1,port=$port_a,mountport=$mount_a,nolock=0,hard,timeo=50" "$vip:/" "$mnt"
echo "failover payload" > "$mnt/failover.bin"
exec 9<>"$mnt/failover.bin"; flock -x 9      # hold a POSIX lock across the failover
dd if=/dev/urandom of="$mnt/big.bin" bs=1M count=8 conv=notrunc status=none  # unstable-ish writes

echo "== kill -9 A, operator takeover on B"
kill -9 "$pid_a"; pid_a=""
until cstat b takeover --json | grep -q '"takeover":true'; do sleep 0.2; done
wait_port "$port_b"
echo "== B: $(cstat b status)"

if [[ $vip == 127.0.0.1 || $vip == localhost ]]; then
  echo "== loopback VIP: remount against gateway B (no keepalived to float the address)"
  flock -u 9; exec 9>&-
  umount -f "$mnt"
  mount -t nfs -o "vers=4.1,port=$port_b,mountport=$mount_b,hard,timeo=50" "$vip:/" "$mnt"
else
  echo "== real VIP: the existing mount reconnects to B; grace lets it reclaim"
fi

echo "== verify state survived the failover"
grep -q "failover payload" "$mnt/failover.bin" || { echo "data lost across failover" >&2; exit 1; }
exec 8<>"$mnt/failover.bin"; flock -x 8 && echo "   re-locked after failover"; flock -u 8; exec 8>&-
echo "new line after takeover" >> "$mnt/failover.bin"
grep -q "new line after takeover" "$mnt/failover.bin" || { echo "post-failover write failed" >&2; exit 1; }

echo "== B metrics"; LIGHTNFS_CTL="$work/b/ctl.sock" "$bin/lightnfs-ctl" metrics | grep '^lightnfs_cluster_' || true

umount -f "$mnt"
kill "$pid_b" 2>/dev/null; pid_b=""
echo
echo "multi-gateway failover VM acceptance ($backend) PASSED"
