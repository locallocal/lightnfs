# lightnfs

A userspace NFS gateway: NFSv3 + NFSv4.1/4.2 on the north side, pluggable storage
backends on the south side (v1: local filesystem). Fully asynchronous C++20
coroutines on an io_uring reactor (epoll fallback).

[中文文档 / Chinese documentation](docs/README.zh.md)

- Design documents: [docs/design/](docs/design/README.md)
- Protocol research: [docs/nfsv3/](docs/nfsv3/README.md), [docs/nfsv4/](docs/nfsv4/README.md)
- Development plan: [docs/development-plan.md](docs/development-plan.md)

Note: the design/research/plan documents are currently written in Chinese.

## Building

Dependencies: CMake ≥ 3.22, Ninja, GCC ≥ 13 or Clang ≥ 17, liburing
(`apt install liburing-dev`; run `scripts/fetch_liburing.sh` for a vendored build
when no system package is available).

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build            # unit + integration tests
./build/bench_nullrpc             # L2 benchmark: single-reactor null-RPC (gate: >=100k rps)
./build/bench_echo                # L1 benchmark: transport-layer echo
./build/bench_fullpath            # L4 benchmark: full path over the in-memory backend
```

Sanitizer builds: `-DLNFS_SANITIZE=address|thread`. Fuzzing (requires clang):
configure with `-DLNFS_BUILD_FUZZ=ON`, then run
`./build/fuzz_handle_request fuzz/corpus`.

## Running

```sh
cp config/lightnfs.toml.example /tmp/lightnfs.toml   # adjust the export path
./build/lightnfsd --check-config --config /tmp/lightnfs.toml
./build/lightnfsd --config /tmp/lightnfs.toml
```

By default the server registers NFSv3 and MOUNTv3 with the local rpcbind; without
rpcbind, clients can mount with explicit `port=`/`mountport=` options. NFSv4.1
clients mount through the pseudo root (`mount -t nfs -o vers=4.1 server:/path ...`).

Admin tooling: `lightnfs-ctl` (unix socket: ping / metrics / dump-errors / drc /
fdcache), `lightnfs-fh` (file-handle decoder with HMAC verification). Prometheus
metrics are also served over HTTP when `[server] metrics_port` is set.

## Acceptance testing

Every milestone ships one-click acceptance scripts:
`scripts/accept_m*_local.sh` run the loopback half without root (userspace NFS
clients driving a real server over TCP, byte-verified against the backing tree);
`scripts/accept_m*_vm.sh` run real kernel mounts on a root VM and are wired into
CI (`m1-acceptance` / `m2-acceptance` / `m3-acceptance` jobs), including cthon04,
fsx, and pynfs integration.

## Status

Phase 3 (M5) complete: read-only NFSv4.1 mounts work — COMPOUND interpreter,
session layer (full EXCHANGE_ID record semantics, exactly-once SEQUENCE slot
table), bitmap attribute layer, pseudo-filesystem namespace, and a minimal
open-state read path; the pynfs 4.1 session groups pass (write-dependent tests
excepted). NFSv3 read-write (phase 2: all 21 procedures, DRC, boot-epoch write
verifier, fd cache, observability tooling) remains under regression protection.

Details: [M3 v4.1 read-only notes](docs/m3-v41-readonly.md),
[M2 read-write notes](docs/m2-readwrite.md),
[M1 read-only notes](docs/m1-readonly.md); security checklist:
[security-checklist.md](docs/security-checklist.md); backend interface review:
[Backend API v1 review](docs/backend-api-review.md). (These documents are in
Chinese.)
