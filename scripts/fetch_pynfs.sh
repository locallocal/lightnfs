#!/usr/bin/env bash
# Fetches and prepares pynfs (development plan §5.4: the 4.1 conformance suite).
# Handles Python >= 3.13 (stdlib xdrlib removal) via the xdrlib3 backport + shim.
#
# usage: fetch_pynfs.sh DEST_DIR
set -euo pipefail

dest=${1:?usage: fetch_pynfs.sh DEST_DIR}

if [[ ! -d $dest/.git ]]; then
  for url in "git://git.linux-nfs.org/projects/bfields/pynfs.git" \
             "https://git.linux-nfs.org/projects/bfields/pynfs.git"; do
    if git clone --depth 1 "$url" "$dest" 2>/dev/null; then break; fi
  done
  [[ -d $dest/.git ]] || { echo "cannot clone pynfs" >&2; exit 1; }
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
