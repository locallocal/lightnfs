#!/bin/sh
# lightnfs container entrypoint: prepare the state directory, then run lightnfsd as the
# unprivileged lightnfs user with only the two capabilities the design needs
# (CAP_DAC_READ_SEARCH for kernel filehandles, CAP_NET_BIND_SERVICE for port 2049/20048),
# the container equivalent of packaging/systemd/lightnfs.service.
#
#   docker run ... lightnfs                       # lightnfsd --config=/etc/lightnfs/lightnfs.toml
#   docker run ... lightnfs --check-config        # bare flags are lightnfsd flags
#   docker exec <ctr> lightnfs-ctl status          # any other command runs verbatim
#
# Environment:
#   LNFS_UID / LNFS_GID   run the server as this uid/gid (default: the image's lightnfs
#                         user).  Set it to the owner of the bind-mounted export tree so a
#                         read-write export is writable.  LNFS_UID=0 keeps root (needed for
#                         identity = "setfsuid").
#   LNFS_STATE_DIR        the [server] state_dir of the config (default /var/lib/lightnfs);
#                         chowned to LNFS_UID:LNFS_GID and set to 0700 before the drop.
#
# Capabilities are best effort: each one missing from the container's bounding set
# (docker's default profile lacks CAP_DAC_READ_SEARCH) is skipped with a warning and
# lightnfsd runs in its documented degraded mode (path-based filehandles).  Add it with
# `--cap-add DAC_READ_SEARCH` / compose `cap_add`.
set -eu

: "${LNFS_UID:=$(id -u lightnfs 2>/dev/null || echo 2049)}"
: "${LNFS_GID:=$(id -g lightnfs 2>/dev/null || echo 2049)}"
: "${LNFS_STATE_DIR:=/var/lib/lightnfs}"

warn() { echo "lightnfs-entrypoint: $*" >&2; }

# No command, or a flag: run the server.
case "${1:-}" in
    "" | -*) set -- lightnfsd "$@" ;;
esac

# Not the server, or already unprivileged (compose `user:` / docker --user): run as is.
if [ "$1" != lightnfsd ] || [ "$(id -u)" != 0 ]; then
    exec "$@"
fi

# State directory: persistent, exclusive, 0700 (deployment guide §3).  Ownership follows
# LNFS_UID so a volume created by an earlier uid is adopted rather than refused.
mkdir -p "$LNFS_STATE_DIR"
if [ "$(stat -c %u:%g "$LNFS_STATE_DIR")" != "$LNFS_UID:$LNFS_GID" ]; then
    chown -R "$LNFS_UID:$LNFS_GID" "$LNFS_STATE_DIR"
fi
if [ "$(stat -c %a "$LNFS_STATE_DIR")" != 700 ]; then
    chmod 0700 "$LNFS_STATE_DIR"
fi

if [ "$LNFS_UID" = 0 ]; then
    exec "$@"
fi

# Ambient capabilities survive the uid change and the execve (also under
# no-new-privileges); probe each one so a missing bounding-set entry degrades instead of
# failing the exec.
caps=""
for cap in dac_read_search net_bind_service; do
    if setpriv --inh-caps="+$cap" --ambient-caps="+$cap" true 2>/dev/null; then
        caps="${caps:+$caps,}+$cap"
    else
        warn "CAP_$(echo "$cap" | tr '[:lower:]' '[:upper:]') not available in this container; continuing without it"
    fi
done

if [ -n "$caps" ]; then
    exec setpriv --reuid="$LNFS_UID" --regid="$LNFS_GID" --clear-groups \
        --inh-caps="$caps" --ambient-caps="$caps" -- "$@"
fi
exec setpriv --reuid="$LNFS_UID" --regid="$LNFS_GID" --clear-groups -- "$@"
