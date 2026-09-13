# syntax=docker/dockerfile:1
# lightnfs container image (multi-stage).
#
# Stage 1 builds a stripped Release lightnfsd / lightnfs-ctl / lightnfs-fh on Ubuntu 24.04
# (GCC 13, CMake 3.28, Ninja, OpenSSL 3 for RPC-over-TLS) with the vendored liburing
# submodule, exactly like packaging/build_common.sh.  Stage 2 is a plain Ubuntu 24.04
# runtime with libssl and the three binaries; the container-tuned configuration
# (docker/lightnfs.toml) and the privilege-dropping entrypoint (docker/entrypoint.sh)
# are copied in.
#
# The build context is the repository root, submodules checked out:
#   git submodule update --init --recursive
#   docker build -f docker/Dockerfile -t lightnfs .
# or, from docker/:  docker compose build
#
# docker/Dockerfile.dockerignore (BuildKit) keeps build trees, .git and the host-built
# liburing objects out of the context.
#
# Build args: LNFS_JOBS (parallel compile jobs, default: all cores),
#             LNFS_UID / LNFS_GID (the image's lightnfs user, default 2049).

# ---- stage 1: build ---------------------------------------------------------------
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The submodules are not fetched here (no network assumptions in the build): fail early
# with the fix if the context was cloned without them.
RUN test -f third_party/liburing/configure -a -f third_party/spdlog/CMakeLists.txt \
         -a -f third_party/ccmd/CMakeLists.txt \
 || { echo "submodules missing: run 'git submodule update --init --recursive' first" >&2; exit 1; }

# Vendored liburing (scripts/fetch_liburing.sh equivalent): CMake prefers the submodule
# once src/liburing.a exists, so no liburing-dev / liburing2 package is needed.
RUN cd third_party/liburing && ./configure >/dev/null && make -C src liburing.a

ARG LNFS_JOBS
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j "${LNFS_JOBS:-$(nproc)}" \
      --target lightnfsd lightnfs-ctl lightnfs-fh \
 && DESTDIR=/stage cmake --install build --prefix /usr/local --strip \
 && mkdir -p /export && /stage/usr/local/bin/lightnfsd --check-config --config docker/lightnfs.toml

# ---- stage 2: runtime -------------------------------------------------------------
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
ARG LNFS_UID=2049
ARG LNFS_GID=2049
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3t64 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid "${LNFS_GID}" lightnfs \
 && useradd --uid "${LNFS_UID}" --gid lightnfs --no-create-home \
      --home-dir /var/lib/lightnfs --shell /usr/sbin/nologin lightnfs \
 && install -d -m 0700 -o lightnfs -g lightnfs /var/lib/lightnfs \
 && install -d -m 0755 /etc/lightnfs /export

COPY --from=build /stage/usr/local/bin/lightnfsd \
                  /stage/usr/local/bin/lightnfs-ctl \
                  /stage/usr/local/bin/lightnfs-fh /usr/local/bin/
COPY docker/lightnfs.toml /etc/lightnfs/lightnfs.toml
COPY --chmod=0755 docker/entrypoint.sh /usr/local/bin/lightnfs-entrypoint

# lightnfs-ctl finds the running server's ctl socket (state_dir/ctl.sock) without --socket.
ENV LIGHTNFS_CTL=/var/lib/lightnfs/ctl.sock

# 2049 NFS (v3 + v4.1/4.2), 20048 MOUNTv3, 9100 Prometheus metrics.  All TCP; no rpcbind.
EXPOSE 2049/tcp 20048/tcp 9100/tcp

# State (boot epoch, HMAC key, reclaim list) must persist across container replacement:
# mount a volume at /var/lib/lightnfs.  The export tree is bind-mounted at /export.
ENTRYPOINT ["lightnfs-entrypoint"]
CMD ["--config=/etc/lightnfs/lightnfs.toml"]

# The ctl socket is 0600 and owned by the server uid; the healthcheck (and `docker exec`)
# run as container root, which needs CAP_DAC_OVERRIDE to connect (docker's default set
# has it; keep it when narrowing with --cap-drop, see docker-compose.yml).
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD ["lightnfs-ctl", "ping"]
