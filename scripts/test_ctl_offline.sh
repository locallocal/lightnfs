#!/usr/bin/env bash
# Offline catalog CLI (plan 12 D3): `lightnfs-ctl catalog <sub> --shared-dir <dir>` end
# to end over the real binary — the flag plumbing, the exit codes and the bootstrap the
# three-instance acceptance script uses (E1).  The command bodies themselves are covered
# by Ctl.OfflineCatalog in lnfs_tests; what only the binary can show is here.
#
# usage: test_ctl_offline.sh [/path/to/lightnfs-ctl]   (default: build/lightnfs-ctl)
set -uo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
ctl=${1:-$repo/build/lightnfs-ctl}
[[ -x $ctl ]] || { echo "no lightnfs-ctl at $ctl" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/lnfs-ctl-offline-XXXXXX")
trap 'rm -rf "$work"' EXIT
shared=$work/shared
mkdir -p "$shared" "$work/a" "$work/b"

failures=0
out=""
rc=0
# run <expected-rc> <description> -- <argv...>
run() {
  local want=$1 what=$2
  shift 3
  out=$("$ctl" "$@" 2>&1)
  rc=$?
  if ((rc != want)); then
    printf 'FAIL %s: exit %d, expected %d\n%s\n' "$what" "$rc" "$want" "$out" >&2
    ((failures++))
  fi
}
# expect_out <description> <substring>
expect_out() {
  if [[ $out != *"$2"* ]]; then
    printf 'FAIL %s: answer does not contain %q\n%s\n' "$1" "$2" "$out" >&2
    ((failures++))
  fi
}

export_block() {  # fsid dir [extra]
  printf '[[export]]\npath = "%s"\nfsid = %s\nbackend = "local"\nclients = ["10.0.0.0/8"]\n%s\n' \
    "$2" "$1" "${3:-}"
}

cluster_head() {  # mode
  printf '[cluster]\nenabled = true\nid = "cluster-offline-test"\nshared_dir = "%s"\nnode = "gw1"\nmode = "%s"\nnode_address = "10.0.0.1:2049"\nexports_source = "catalog"\n' \
    "$shared" "$1"
}

{
  printf '[server]\nport = 2049\n'
  cluster_head active-active
  export_block 1 "$work/a" 'nodes = ["gw1", "gw2"]'
  export_block 2 "$work/b" 'nodes = ["gw2", "gw1"]'
} >"$work/local.toml"

# Same exports, but fsid 2 without `nodes`: legal under failover, refused under
# active-active — the mode comes from the imported file's own [cluster] section.
{
  printf '[server]\nport = 2049\n'
  cluster_head active-active
  export_block 1 "$work/a" 'nodes = ["gw1"]'
  export_block 2 "$work/b"
} >"$work/loose_aa.toml"
sed 's/mode = "active-active"/mode = "failover"/' "$work/loose_aa.toml" >"$work/loose_fo.toml"

# --- argument handling -----------------------------------------------------------------
run 2 "no --shared-dir" -- catalog show
expect_out "no --shared-dir" "--shared-dir <dir> is required"
run 2 "shared dir does not exist" -- catalog show --shared-dir "$work/missing"
expect_out "shared dir does not exist" "is not a directory"
run 2 "bare catalog prints help" -- catalog
expect_out "bare catalog prints help" "offline shared export catalog administration"

# --- an empty shared directory ---------------------------------------------------------
run 0 "show with no catalog" -- catalog show --shared-dir "$shared"
[[ $out == "catalog: none" ]] || { echo "FAIL show with no catalog: $out" >&2; ((failures++)); }
run 0 "show --json with no catalog" -- catalog show --shared-dir "$shared" --json
[[ $out == '{"catalog":null}' ]] || { echo "FAIL show --json: $out" >&2; ((failures++)); }
run 1 "diff cannot default the versions offline" -- catalog diff --shared-dir "$shared"
expect_out "diff cannot default the versions offline" "no gateway runs in this process"

# --- bootstrap (what accept_active_active_local.sh publishes v1 with) -------------------
run 0 "import --dry-run" -- catalog import --from-local "$work/local.toml" --shared-dir "$shared" --dry-run
expect_out "import --dry-run" "would commit v1 (current none): added=1,2"
[[ -e $shared/catalog.toml ]] && { echo "FAIL: --dry-run wrote catalog.toml" >&2; ((failures++)); }

run 0 "import" -- catalog import --from-local "$work/local.toml" --shared-dir "$shared" \
  --comment "first version, offline"
expect_out "import" "catalog v1 imported (was none): added=1,2"
run 0 "show after import" -- catalog show --shared-dir "$shared"
expect_out "show after import" "version=1 exports=2"
expect_out "audit trail" "updated_by=offline uid=$(id -u) comment=first version, offline"
expect_out "show after import" "fsid=1 path=$work/a backend=local nodes=gw1,gw2 disabled=no"

# --shared-dir ahead of the subcommand reaches the leaf just like --socket does.
run 0 "--shared-dir before the subcommand" -- --shared-dir "$shared" catalog show
expect_out "--shared-dir before the subcommand" "version=1 exports=2"
run 0 "-d before the subcommand" -- -d "$shared" catalog show
expect_out "-d before the subcommand" "version=1 exports=2"

# --- the active-active rule follows the imported file's mode ---------------------------
run 1 "active-active needs nodes on every export" -- catalog import --from-local "$work/loose_aa.toml" \
  --shared-dir "$shared"
expect_out "active-active needs nodes on every export" 'export fsid=2: nodes is required under active-active'
run 0 "the same exports pass under failover" -- catalog import --from-local "$work/loose_fo.toml" \
  --shared-dir "$shared" --comment v2
expect_out "the same exports pass under failover" "catalog v2 imported (was v1): added=- removed=- disabled=- enabled=- nodes_changed=1,2"
run 1 "--active-active forces the rule" -- catalog import --from-local "$work/loose_fo.toml" \
  --shared-dir "$shared" --active-active
expect_out "--active-active forces the rule" 'nodes is required under active-active'

# --from-local names a configuration: a catalog document is refused, and the same
# document goes in as a positional argument.
run 1 "--from-local refuses a catalog document" -- catalog import --from-local "$shared/catalog.toml" \
  --shared-dir "$shared"
expect_out "--from-local refuses a catalog document" "is a catalog document, not a gateway configuration"
run 0 "a catalog document imports positionally" -- catalog import "$shared/catalog.toml" --shared-dir "$shared" \
  --dry-run
expect_out "a catalog document imports positionally" "would commit v3 (current v2)"

# --- history / diff / rollback ----------------------------------------------------------
run 0 "history" -- catalog history --shared-dir "$shared"
expect_out "history" "version=1 exports=2"
expect_out "history" "comment=v2 current=yes"
run 0 "diff 1 2" -- catalog diff 1 2 --shared-dir "$shared"
expect_out "diff 1 2" $'from=1 to=2\nadded=-'
expect_out "diff 1 2" "nodes_changed=1,2"
run 0 "diff none 1" -- catalog diff none 1 --shared-dir "$shared"
expect_out "diff none 1" $'from=0 to=1\nadded=1,2'
run 1 "diff of an unknown version" -- catalog diff 1 9 --shared-dir "$shared"
expect_out "diff of an unknown version" "version 9 is neither current"

run 1 "rollback to the current version" -- catalog rollback 2 --shared-dir "$shared"
expect_out "rollback to the current version" "version 2 is the current catalog"
run 0 "rollback" -- catalog rollback 1 --shared-dir "$shared"
expect_out "rollback" "catalog v3 committed: rollback to v1 (was v2)"
run 0 "diff 1 3 is empty" -- catalog diff 1 3 --shared-dir "$shared"
expect_out "diff 1 3 is empty" $'added=-\nremoved=-\ndisabled=-\nenabled=-\nnodes_changed=-\ndynamic_changed=-\nrejected=-'

# --- status and the --json rendering ----------------------------------------------------
run 0 "status with no gateway reporting" -- catalog status --shared-dir "$shared"
[[ $out == "latest=3" ]] || { echo "FAIL status: $out" >&2; ((failures++)); }
run 0 "status --json" -- catalog status --shared-dir "$shared" --json
[[ $out == '{"latest":3,"nodes":[]}' ]] || { echo "FAIL status --json: $out" >&2; ((failures++)); }
run 1 "an error answers JSON too" -- catalog rollback 99 --shared-dir "$shared" --json
expect_out "an error answers JSON too" '{"error":"version 99 is neither current'

# `apply` needs a running gateway and is not part of the offline tree.
run 1 "apply is not an offline subcommand" -- catalog apply --shared-dir "$shared"
expect_out "apply is not an offline subcommand" "not found"

if ((failures == 0)); then
  echo "test_ctl_offline.sh PASSED"
  exit 0
fi
echo "test_ctl_offline.sh FAILED ($failures)" >&2
exit 1
