#!/usr/bin/env bash
# Rolling maintenance of one active-active gateway (design 10 §10.7, plan 12 D2):
#
#   cluster_roll.sh evacuate <node> [--to <node>]   # migrate every export <node> serves
#                                                    # to --to, or to the next live node
#                                                    # in that export's `nodes` list, and
#                                                    # wait until <node> serves nothing
#   cluster_roll.sh restore  <node>                  # migrate every export whose
#                                                    # nodes[0] is <node> back to it
#
# Everything goes through `lightnfs-ctl cluster status --json` (the per-export table
# with `nodes`, `owner`, the gateway's `peers_alive`) and `cluster migrate <fsid>
# <node>`, which runs on the export's current owner.  So the script needs one ctl
# socket per gateway it talks to:
#
#   --sockets gw1=/run/lightnfs/ctl.sock,gw2=ssh://...   # node=socket pairs, or
#   LNFS_CTL_SOCKETS=gw1=/path,gw2=/path                  # the same from the environment
#
# A socket value is what `lightnfs-ctl --socket` accepts (a Unix socket path; on a
# remote host, forward it first).  `evacuate` needs the evacuated node's socket and,
# for the final wait, only that one; `restore` needs the socket of every current owner.
# Every step stops at the first failure and prints the owner table as it stands.
#
# usage: cluster_roll.sh [--sockets MAP] [--ctl PATH] [--timeout SECONDS] \
#            evacuate <node> [--to <node>] | restore <node>
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
ctl_bin=${LIGHTNFS_CTL_BIN:-}
if [[ -z $ctl_bin ]]; then
  for c in "$repo/build-rel/lightnfs-ctl" "$repo/build/lightnfs-ctl" lightnfs-ctl; do
    if command -v "$c" >/dev/null 2>&1 || [[ -x $c ]]; then ctl_bin=$c; break; fi
  done
fi
sockets=${LNFS_CTL_SOCKETS:-}
timeout_s=${LNFS_ROLL_TIMEOUT:-30}
to=""
cmd=""
node=""

usage() {
  sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-2}"
}

while (($# > 0)); do
  case $1 in
    --sockets) sockets=$2; shift 2 ;;
    --sockets=*) sockets=${1#--sockets=}; shift ;;
    --ctl) ctl_bin=$2; shift 2 ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --to) to=$2; shift 2 ;;
    --to=*) to=${1#--to=}; shift ;;
    -h | --help) usage 0 ;;
    evacuate | restore)
      cmd=$1
      node=${2:-}
      [[ -n $node ]] || { echo "cluster_roll.sh: $cmd needs a node name" >&2; usage; }
      shift 2 ;;
    *) echo "cluster_roll.sh: unknown argument $1" >&2; usage ;;
  esac
done
[[ -n $cmd ]] || usage
[[ -n $ctl_bin ]] || { echo "cluster_roll.sh: lightnfs-ctl not found (use --ctl)" >&2; exit 2; }

# socket_of <node>: the ctl socket for that gateway from the map, else LIGHTNFS_CTL
# when it is the only gateway we were told about.
socket_of() {
  local entry
  IFS=',' read -ra entries <<<"$sockets"
  for entry in "${entries[@]}"; do
    [[ ${entry%%=*} == "$1" ]] && { echo "${entry#*=}"; return 0; }
  done
  [[ -n ${LIGHTNFS_CTL:-} && -z $sockets ]] && { echo "$LIGHTNFS_CTL"; return 0; }
  echo "cluster_roll.sh: no ctl socket known for node $1 (use --sockets)" >&2
  return 1
}

ctl() {  # <node> <args...> → the answer; a ctl error line fails the step
  local sock
  sock=$(socket_of "$1") || return 1
  LIGHTNFS_CTL=$sock "$ctl_bin" "${@:2}"
}

# status_json <node> → the gateway's `cluster status --json`
status_json() { ctl "$1" cluster status --json; }

# Fields out of the status JSON, one per line, via python3 (no jq dependency).
# rows <json> → "fsid role owner nodes(comma-joined)" per export
rows() {
  python3 -c '
import json, sys
st = json.load(sys.stdin)
for e in st["exports"]:
    print(e["fsid"], e["role"], e["owner"] or "-", ",".join(e["nodes"]))
' <<<"$1"
}
alive_peers() {  # <json> → space-separated live peers
  python3 -c 'import json,sys; print(" ".join(json.load(sys.stdin)["peers_alive"] or []))' <<<"$1"
}

print_table() {  # <node>: the owner table as that gateway sees it
  echo "-- owner table as seen by $1:"
  ctl "$1" cluster status || true
}

fail() {
  echo "cluster_roll.sh: $*" >&2
  [[ -n $node ]] && print_table "$node" >&2 || true
  exit 1
}

# migrate <owner> <fsid> <target>: one export, then wait until the owner reports it
# remote and served by the target.
migrate() {
  local owner=$1 fsid=$2 target=$3 out deadline row
  out=$(ctl "$owner" cluster migrate "$fsid" "$target" --json) ||
    fail "migrate $fsid $owner → $target failed: $out"
  grep -q '"migrate":true' <<<"$out" || fail "migrate $fsid $owner → $target refused: $out"
  echo "migrating fsid $fsid: $owner → $target"
  # Wait until the target itself serves the export, not merely until the source has let
  # go: the handover is source-writes-owner-then-releases, target-takes-on-its-next-tick
  # (plan 12 D1), and the next roll step must not race into the gap between the two.
  deadline=$((SECONDS + timeout_s))
  while :; do
    row=$(rows "$(status_json "$target")" | awk -v f="$fsid" '$1 == f')
    [[ $row == "$fsid active $target "* ]] && { echo "  fsid $fsid served by $target"; return 0; }
    ((SECONDS < deadline)) || fail "fsid $fsid not served by $target within ${timeout_s}s (row: $row)"
    sleep 0.2
  done
}

case $cmd in
  evacuate)
    st=$(status_json "$node") || fail "cannot read $node's status"
    live=" $(alive_peers "$st") "
    if [[ -n $to ]]; then
      [[ $live == *" $to "* ]] || fail "target $to is not a live peer (alive:$live)"
    fi
    moved=0
    while read -r fsid role owner nodes; do
      [[ $role == active || $role == activating ]] || continue
      target=$to
      if [[ -z $target ]]; then
        # The next live node after <node> in the export's own list, wrapping around.
        IFS=',' read -ra order <<<"$nodes"
        for cand in "${order[@]}" "${order[@]}"; do
          [[ $cand == "$node" ]] && continue
          [[ $live == *" $cand "* ]] && { target=$cand; break; }
        done
      fi
      [[ -n $target ]] || fail "no live successor for fsid $fsid (nodes=$nodes, alive:$live)"
      migrate "$node" "$fsid" "$target"
      moved=$((moved + 1))
    done < <(rows "$st")
    # Nothing left in service here.
    deadline=$((SECONDS + timeout_s))
    while :; do
      left=$(rows "$(status_json "$node")" | awk '$2 == "active" || $2 == "activating" || $2 == "draining"' | wc -l)
      ((left == 0)) && break
      ((SECONDS < deadline)) || fail "$node still serves $left export(s)"
      sleep 0.2
    done
    echo "evacuated $node: $moved export(s) moved, none in service"
    print_table "$node"
    ;;
  restore)
    st=$(status_json "$node") || fail "cannot read $node's status"
    restored=0
    while read -r fsid role owner nodes; do
      [[ ${nodes%%,*} == "$node" ]] || continue
      case $role in
        active | activating) continue ;;
        remote) ;;
        *) fail "fsid $fsid is $role (owner $owner): nothing to restore from" ;;
      esac
      migrate "$owner" "$fsid" "$node"
      restored=$((restored + 1))
    done < <(rows "$st")
    echo "restored $node: $restored export(s) back"
    print_table "$node"
    ;;
esac
