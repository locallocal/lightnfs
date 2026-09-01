#!/usr/bin/env bash
# Fetches and prepares pynfs (development plan §5.4: the 4.1 conformance suite).
# Handles Python >= 3.13 (stdlib xdrlib removal) via the xdrlib3 backport + shim.
#
# usage: fetch_pynfs.sh DEST_DIR
# Pinned to a fixed upstream commit (plan doc 10 §7.3); LNFS_FETCH_HEAD=1 takes the
# current upstream HEAD instead (for bumping the pin).
set -euo pipefail

dest=${1:?usage: fetch_pynfs.sh DEST_DIR}

pin=cd4701827a8261fedbfb4c6e39029fb9671321a6  # upstream HEAD, 2026-09-01
urls=("https://github.com/linux-nfs/pynfs.git"
      "git://git.linux-nfs.org/projects/bfields/pynfs.git"
      "https://git.linux-nfs.org/projects/bfields/pynfs.git")

if [[ ! -d $dest/.git ]]; then
  cloned=""
  for url in "${urls[@]}"; do
    if [[ ${LNFS_FETCH_HEAD:-0} == 1 ]]; then
      git clone --depth 1 "$url" "$dest" 2>/dev/null && { cloned=$url; break; }
    else
      if git init -q "$dest" &&
        git -C "$dest" fetch -q --depth 1 "$url" "$pin" 2>/dev/null &&
        git -C "$dest" checkout -q FETCH_HEAD; then
        cloned=$url
        break
      fi
      rm -rf "$dest/.git"  # failed attempt: drop only the git metadata
    fi
  done
  [[ -n $cloned ]] || { echo "cannot clone pynfs @$pin" >&2; exit 1; }
fi

python3 -c "import ply" 2>/dev/null || \
  pip install --user --break-system-packages ply >/dev/null
if ! python3 -c "import xdrlib" 2>/dev/null; then
  pip install --user --break-system-packages xdrlib3 >/dev/null
  site=$(python3 -c "import site; print(site.getusersitepackages())")
  mkdir -p "$site"
  printf 'from xdrlib3 import *\nfrom xdrlib3 import Packer, Unpacker, Error, ConversionError\n' \
    > "$site/xdrlib.py"
fi

(cd "$dest" && python3 setup.py build >/dev/null 2>&1 || true)
[[ -f $dest/nfs4.1/xdrdef/nfs4_pack.py ]] || { echo "pynfs build failed" >&2; exit 1; }
echo "pynfs ready at $dest"
