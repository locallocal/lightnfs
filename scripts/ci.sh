#!/usr/bin/env bash
# One-stop CI driver (plan doc 10 §7.3): the build matrix the README promises —
# GCC/Clang × Debug/Release × ASAN+UBSAN × TSAN × epoll ring × fuzz regress — plus the
# errmap doc-drift gate, the format gate, and (nightly) the bench floor gate and a 1 h
# fuzz run.  Runs locally or from any scheduler; the repository carries no hosted CI.
#
# usage: ci.sh            # full matrix (per-change gate)
#        ci.sh quick      # default build + ctest only
#        ci.sh nightly    # full matrix + bench gate + 1 h fuzz
#
# env: LNFS_JOBS  parallelism for builds and ctest (default: half the cores)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
jobs=${LNFS_JOBS:-$(($(nproc) / 2))}
((jobs > 0)) || jobs=1
mode=${1:-full}

have_clang=""
command -v clang++ >/dev/null && have_clang=1

failures=()
note() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
run_step() {  # name cmd...
  local name=$1
  shift
  note "$name"
  if "$@"; then echo "-- $name OK"; else failures+=("$name"); echo "-- $name FAILED" >&2; fi
}

configure_build_test() {  # dir cmake-args...
  local dir=$repo/$1
  shift
  cmake -S "$repo" -B "$dir" -G Ninja "$@" >/dev/null
  cmake --build "$dir" -j "$jobs"
  ctest --test-dir "$dir" -j "$jobs" --output-on-failure
}

# 1. Default (GCC Debug) — the everyday configuration.
run_step "build+test debug/gcc" configure_build_test build -DCMAKE_BUILD_TYPE=Debug

if [[ $mode == quick ]]; then
  ((${#failures[@]} == 0)) && { echo "ci.sh quick PASSED"; exit 0; }
  echo "ci.sh quick FAILED: ${failures[*]}" >&2
  exit 1
fi

# 2. Release (GCC) — what bench/packaging ship.
run_step "build+test release/gcc" configure_build_test build-rel \
  -DCMAKE_BUILD_TYPE=Release

# 3. Release + epoll default ring — the io_uring-less fallback path.
run_step "build+test release/epoll" configure_build_test build-epoll \
  -DCMAKE_BUILD_TYPE=Release -DLNFS_RING=epoll

if [[ -n $have_clang ]]; then
  # 4. ASAN+UBSAN (clang, Debug).
  run_step "build+test asan/clang" configure_build_test build-asan \
    -DCMAKE_BUILD_TYPE=Debug -DLNFS_SANITIZE=address \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  # 5. TSAN (clang, Debug) — carries the §1.2-class race reproducers.
  run_step "build+test tsan/clang" configure_build_test build-tsan \
    -DCMAKE_BUILD_TYPE=Debug -DLNFS_SANITIZE=thread \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  # 6. Fuzz: build the libFuzzer targets + a seeded smoke run (120 s total).
  run_step "fuzz smoke (120s)" "$repo/scripts/fuzz.sh" smoke
else
  echo "!! clang++ not found: skipping ASAN, TSAN and libFuzzer legs" >&2
fi

# 7. Gates that need no build directory.
run_step "errmap doc-drift check" \
  python3 "$repo/scripts/gen_errmap_cases.py" --check
run_step "clang-format check" "$repo/scripts/format_check.sh"
# libgfapi signature drift (plan doc 10 §5.3): the gluster backend loads the library
# at runtime, so this is the only build-time check; skips where the headers are absent.
run_step "gfapi ABI check" "$repo/scripts/check_gfapi_abi.sh"
# Lustre uapi drift (design 06 §6.5): the lustre backend speaks ioctls to the kernel
# client directly; skips where lustre_user.h is absent.
run_step "llapi ABI check" "$repo/scripts/check_llapi_abi.sh"

if [[ $mode == nightly ]]; then
  run_step "bench floor gate" "$repo/scripts/bench_gate.sh" "$repo/build-rel"
  [[ -n $have_clang ]] && run_step "fuzz nightly (1h)" "$repo/scripts/fuzz.sh" nightly
fi

echo
if ((${#failures[@]} == 0)); then
  echo "ci.sh $mode PASSED"
else
  echo "ci.sh $mode FAILED: ${failures[*]}" >&2
  exit 1
fi
