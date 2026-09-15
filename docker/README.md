# lightnfs in a container

Files in this directory:

| File | Purpose |
|------|---------|
| `Dockerfile` | Multi-stage image: Release build on Ubuntu 24.04 (GCC 13, vendored liburing, OpenSSL for RPC-over-TLS), runtime = Ubuntu 24.04 + `libssl3t64` + the three binaries. Build context is the repository root. |
| `Dockerfile.dockerignore` | BuildKit context filter (build trees, `.git`, host-built liburing objects). |
| `entrypoint.sh` | Prepares the state directory, then drops to the `lightnfs` user keeping only `CAP_DAC_READ_SEARCH` + `CAP_NET_BIND_SERVICE` (ambient caps via `setpriv`); the container equivalent of `packaging/systemd/lightnfs.service`. |
| `lightnfs.toml` | Container-tuned config: `state_dir = /var/lib/lightnfs`, one local export at `/export`, `rpcbind = false`, metrics on 9100. Baked into the image and bind-mounted by compose. |
| `docker-compose.yml` | Single-gateway deployment: state volume, export bind mount, published ports, least-privilege capability set, read-only root, healthcheck. |

## Quick start

```sh
git submodule update --init --recursive     # the image builds the vendored deps
cd docker
mkdir -p export && sudo chown 2049:2049 export   # or: echo "LNFS_UID=$(id -u)" > .env
docker compose up -d --build
docker compose exec lightnfs lightnfs-ctl status
docker compose logs -f
```

Mount from a client:

```sh
mount -t nfs -o vers=4.1 <host>:/export /mnt
mount -t nfs -o vers=4.2 <host>:/export /mnt
mount -t nfs -o vers=3,port=2049,mountport=20048 <host>:/export /mnt   # no rpcbind
```

Standalone `docker run`, same image:

```sh
docker build -f docker/Dockerfile -t lightnfs:1.3.0 .        # from the repository root
docker run -d --name lightnfs \
  --cap-drop ALL --cap-add CHOWN --cap-add DAC_OVERRIDE --cap-add FOWNER \
  --cap-add SETUID --cap-add SETGID --cap-add DAC_READ_SEARCH --cap-add NET_BIND_SERVICE \
  --security-opt no-new-privileges \
  -p 2049:2049 -p 20048:20048 \
  -v lightnfs-state:/var/lib/lightnfs -v /srv/data:/export \
  lightnfs:1.3.0
docker exec lightnfs lightnfs-ctl ping
```

## Knobs

| Variable | Default | Meaning |
|----------|---------|---------|
| `LNFS_EXPORT` (compose) | `./export` | Host directory bind-mounted at `/export`. |
| `LNFS_UID` / `LNFS_GID` | `2049` | uid/gid the server runs as. Match the export's owner for a read-write export; `LNFS_UID=0` keeps root (needed for `identity = "setfsuid"`). |
| `LNFS_IMAGE` (compose) | `lightnfs:1.3.0` | Image tag to build/run. |
| `LNFS_JOBS` (build arg) | all cores | Parallel compile jobs: `docker build --build-arg LNFS_JOBS=8 …`. |

Put compose variables in `docker/.env` (git-ignored).

## Notes

- **State must persist.** `/var/lib/lightnfs` holds the boot epoch, the filehandle HMAC
  key and the v4 reclaim list. Recreating it invalidates every client's handles and
  defeats grace-period reclaim; compose keeps it in the `lightnfs-state` volume.
- **io_uring vs. epoll.** Docker's default seccomp profile blocks `io_uring_*`, so
  `ring = "auto"` starts the epoll fallback ring (the log line `runtime: … ring=epoll`
  shows which). To run on io_uring, use a seccomp profile that allows
  `io_uring_setup/enter/register`, or `security_opt: [seccomp=unconfined]` on a trusted
  host.
- **Capabilities.** Docker's default set lacks `CAP_DAC_READ_SEARCH`; without it the
  entrypoint warns and the local backend uses path-based filehandles (see
  `docs/guide/deployment.md`). The compose file adds it. `CAP_NET_BIND_SERVICE` is only needed
  for ports < 1024 inside the container. The other capabilities in the compose list are
  used by the entrypoint before it drops to `LNFS_UID`, plus `CAP_DAC_OVERRIDE` for
  container root (healthcheck, `docker compose exec … lightnfs-ctl`) to reach the 0600
  ctl socket; the server process itself never holds them.
- **Networking.** lightnfs is TCP-only and does not need rpcbind, so published ports
  work: NFSv4.1/4.2 uses 2049 alone; NFSv3 clients pass `port=` and `mountport=`.
  Clients keep their source address through Docker's NAT, so the export `clients` CIDR
  list applies as usual; connections from the Docker host itself arrive from the bridge
  gateway address. `network_mode: host` is the simpler choice for a dedicated gateway.
- **Configuration changes.** Edit `docker/lightnfs.toml`, then
  `docker compose exec lightnfs lightnfs-ctl reload` for hot-reloadable keys or
  `docker compose restart` for the rest. Validate first:
  `docker compose run --rm lightnfs --check-config`.
- **Other backends.** The image contains no `libgfapi` / `libcephfs`; a gluster or
  cephfs export needs those runtime libraries added to the image (they are `dlopen`ed),
  a lustre export needs the client mount bind-mounted into the container.
