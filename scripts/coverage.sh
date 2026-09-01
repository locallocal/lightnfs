#!/usr/bin/env bash
# Line/function coverage for the unit suite via clang source-based coverage
# (plan doc 10 §7.3 evaluated llvm-cov: adopted as an on-demand report, not a gate).
#
# usage: coverage.sh            # builds build-cov, runs lnfs_tests, prints a summary
#        coverage.sh --html     # additionally writes build-cov/coverage-html/
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build=$repo/build-cov
jobs=${LNFS_JOBS:-$(($(nproc) / 2))}
((jobs > 0)) || jobs=1

for tool in clang++ llvm-profdata llvm-cov; do
  command -v $tool >/dev/null || { echo "coverage.sh: $tool not installed" >&2; exit 1; }
done

cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" >/dev/null
cmake --build "$build" -j "$jobs" --target lnfs_tests >/dev/null

LLVM_PROFILE_FILE="$build/lnfs_tests.%p.profraw" "$build/lnfs_tests"
llvm-profdata merge -sparse "$build"/lnfs_tests.*.profraw -o "$build/lnfs_tests.profdata"
rm -f "$build"/lnfs_tests.*.profraw

llvm-cov report "$build/lnfs_tests" -instr-profile="$build/lnfs_tests.profdata" \
  -ignore-filename-regex='(third_party|tests)/' "$repo/src"
if [[ ${1:-} == --html ]]; then
  llvm-cov show "$build/lnfs_tests" -instr-profile="$build/lnfs_tests.profdata" \
    -ignore-filename-regex='(third_party|tests)/' -format=html \
    -output-dir="$build/coverage-html" "$repo/src"
  echo "html report: $build/coverage-html/index.html"
fi
