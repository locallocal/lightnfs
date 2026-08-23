#!/usr/bin/env bash
# Three-layer benchmark regression gate (design 02 §2.8). Runs `lightnfs-ctl bench`
# echo / nullrpc / fullpath(GETATTR, READ4k) and compares the measured rps against
# bench/baseline.txt x LNFS_BENCH_FLOOR.  Exit 1 on any miss.
#
# usage: bench_gate.sh [BUILD_DIR]      (env: LNFS_BENCH_FLOOR=0.5, LNFS_BENCH_CALLS=20000)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$repo/build-rel}
floor=${LNFS_BENCH_FLOOR:-0.5}
calls=${LNFS_BENCH_CALLS:-20000}
baseline="$repo/bench/baseline.txt"

ref() { awk -v k="$1" '$1==k {print $2}' "$baseline"; }
rps_of() { sed -nE 's/.* = ([0-9]+) rps.*/\1/p' <<<"$1" | tail -1; }

run() {  # $1 key, $2.. command
  local key=$1; shift
  local out
  out=$("$@" 2>/dev/null | tail -1)
  echo "   $out"
  local got; got=$(rps_of "$out")
  local want; want=$(ref "$key")
  [[ -n $got && -n $want ]] || { echo "bench_gate: cannot parse $key" >&2; return 1; }
  local min; min=$(awk -v w="$want" -v f="$floor" 'BEGIN{printf "%d", w*f}')
  if (( got < min )); then
    echo "bench_gate: $key = $got rps < floor $min (baseline $want x $floor)" >&2
    return 1
  fi
  echo "   ok: $key $got rps >= $min (baseline $want x $floor)"
}

status=0
run nullrpc       "$build/lightnfs-ctl" bench nullrpc  1 4 "$calls" 32      || status=1
run echo          "$build/lightnfs-ctl" bench echo     1 4 "$calls" 32 128  || status=1
run fullpath      "$build/lightnfs-ctl" bench fullpath 1 4 "$calls" 32      || status=1
run fullpath_read "$build/lightnfs-ctl" bench fullpath 1 4 "$calls" 32 read || status=1
[[ $status -eq 0 ]] && echo "bench_gate PASSED (floor $floor)" || echo "bench_gate FAILED" >&2
exit $status
