#!/usr/bin/env bash
# Formatting gate (plan doc 10 §7.3): clang-format per .clang-format, plus the
# comment-placement rule (a `//` comment sits on the line above its code, never after
# it — scripts/trailing_comments.py).  Checks every tracked C++ file outside
# third_party/; generated .inc files are exempt (they carry clang-format off markers
# but are not written by clang-format).
#
# usage: format_check.sh          # check, non-zero on drift or a trailing comment
#        format_check.sh --fix    # move trailing comments, then rewrite in place
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

fmt=${CLANG_FORMAT:-clang-format}
if ! command -v "$fmt" >/dev/null; then
  echo "format_check.sh: $fmt not installed — skipping (install clang-format to enforce)"
  exit 0
fi

mapfile -t files < <(git ls-files '*.cpp' '*.hpp' ':!:third_party/**' ':!:tests/*.inc')
if [[ ${1:-} == --fix ]]; then
  python3 "$repo/scripts/trailing_comments.py" --fix "${files[@]}"
  "$fmt" -i "${files[@]}"
  echo "format_check: reformatted ${#files[@]} files"
else
  "$fmt" --dry-run -Werror "${files[@]}"
  echo "format_check: ${#files[@]} files clean"
  python3 "$repo/scripts/trailing_comments.py" --check "${files[@]}"
fi
