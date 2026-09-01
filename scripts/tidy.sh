#!/usr/bin/env bash
# clang-tidy over src/ using the compile_commands.json of an existing build directory
# (plan doc 10 §7.3 evaluated clang-tidy: adopted with the narrow check set in
# .clang-tidy — bugprone/performance/concurrency, no style churn).
#
# usage: tidy.sh [build-dir]   # default: build
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$repo/build}
jobs=${LNFS_JOBS:-$(($(nproc) / 2))}
((jobs > 0)) || jobs=1

command -v clang-tidy >/dev/null ||
  { echo "tidy.sh: clang-tidy not installed — skipping"; exit 0; }
[[ -f $build/compile_commands.json ]] ||
  { echo "tidy.sh: $build/compile_commands.json missing (configure the build first)" >&2; exit 1; }

mapfile -t files < <(git -C "$repo" ls-files 'src/*.cpp')
if command -v run-clang-tidy >/dev/null; then
  run-clang-tidy -quiet -p "$build" -j "$jobs" "${files[@]/#/$repo/}"
else
  for f in "${files[@]}"; do clang-tidy -quiet -p "$build" "$repo/$f"; done
fi
echo "tidy: ${#files[@]} files checked"
