#!/usr/bin/env bash
# libFuzzer driver (plan doc 10 §7.2/§7.3): builds the clang fuzz targets and runs them
# on a time budget, backing the README's "120 s seeded run per change, 1 h nightly" with
# an actual entry point.  Seed corpora (fuzz/seed/<target>/, checked in) feed every run;
# findings grow the machine-local corpus (fuzz/corpus/<target>/, gitignored).
#
# usage: fuzz.sh smoke                 # 120 s total, split across all targets
#        fuzz.sh nightly              # 1 h total, split across all targets
#        fuzz.sh run <target> [secs]  # one target (default 300 s)
#        fuzz.sh minimize             # corpus minimization (-merge) per target
#        fuzz.sh regress              # replay seeds+corpus through the non-clang build
#
# env: LNFS_FUZZ_BUILD_DIR (default build-fuzz), LNFS_JOBS (default nproc/2)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build=${LNFS_FUZZ_BUILD_DIR:-$repo/build-fuzz}
jobs=${LNFS_JOBS:-$(($(nproc) / 2))}
((jobs > 0)) || jobs=1

targets=(handle_request file_handle config record_stream v4_attrs objid_local)
# target -> dictionary (empty = none)
dict_for() {
  case $1 in
    handle_request) echo "$repo/fuzz/dict/rpc.dict" ;;
    file_handle) echo "$repo/fuzz/dict/file_handle.dict" ;;
    config) echo "$repo/fuzz/dict/config.dict" ;;
    record_stream) echo "$repo/fuzz/dict/record_stream.dict" ;;
    v4_attrs) echo "$repo/fuzz/dict/v4_attrs.dict" ;;
    *) echo "" ;;
  esac
}

build_targets() {
  command -v clang++ >/dev/null || { echo "fuzz.sh: clang++ required" >&2; exit 1; }
  cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLNFS_BUILD_FUZZ=ON \
    >/dev/null
  cmake --build "$build" -j "$jobs" >/dev/null
}

run_one() {  # target seconds
  local t=$1 secs=$2
  local corpus="$repo/fuzz/corpus/$t" seed="$repo/fuzz/seed/$t"
  mkdir -p "$corpus"
  local args=(-max_total_time="$secs" -print_final_stats=1)
  local dict; dict=$(dict_for "$t")
  [[ -n $dict && -f $dict ]] && args+=(-dict="$dict")
  echo "== fuzz_$t (${secs}s) =="
  "$build/fuzz_$t" "${args[@]}" "$corpus" "$seed" 2>&1 |
    grep -E "^(#|==|Done|stat::|SUMMARY|DEDUP|.*ERROR)" | tail -6
}

case ${1:-smoke} in
  smoke|nightly)
    total=$([[ ${1:-smoke} == nightly ]] && echo 3600 || echo 120)
    per=$((total / ${#targets[@]}))
    build_targets
    for t in "${targets[@]}"; do run_one "$t" "$per"; done
    echo "fuzz.sh ${1:-smoke} done (${per}s per target)"
    ;;
  run)
    t=${2:?usage: fuzz.sh run <target> [secs]}
    build_targets
    run_one "$t" "${3:-300}"
    ;;
  minimize)
    build_targets
    for t in "${targets[@]}"; do
      corpus="$repo/fuzz/corpus/$t" seed="$repo/fuzz/seed/$t"
      [[ -d $corpus ]] || continue
      min="$corpus.min"
      rm -rf "$min" && mkdir -p "$min"
      echo "== minimize $t =="
      "$build/fuzz_$t" -merge=1 "$min" "$corpus" "$seed" 2>&1 | tail -2
      rm -rf "$corpus" && mv "$min" "$corpus"
    done
    ;;
  regress)
    # Replay everything through the sanitizer-free regress binaries (any compiler).
    rbuild=${LNFS_BUILD_DIR:-$repo/build}
    cmake --build "$rbuild" -j "$jobs" >/dev/null
    for t in "${targets[@]}"; do
      "$rbuild/fuzz_regress_$t" "$repo/fuzz/seed/$t" "$repo/fuzz/corpus/$t" | tail -1
    done
    ;;
  *)
    echo "usage: fuzz.sh smoke|nightly|run <target> [secs]|minimize|regress" >&2
    exit 2
    ;;
esac
