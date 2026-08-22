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
rpcbind, clients can mount with explicit `port=`/`mountport=` options. NFSv4.1/4.2
clients mount through the pseudo root (`mount -t nfs -o vers=4.2 server:/path ...`;
4.2 adds SEEK/ALLOCATE/DEALLOCATE, server-side COPY and CLONE on top of the 4.1
state machine, each advertised per export from a startup capability probe).

Admin tooling: `lightnfs-ctl` (unix socket: ping / metrics / dump-errors / drc /
fdcache), `lightnfs-fh` (file-handle decoder with HMAC verification). Prometheus
metrics are also served over HTTP when `[server] metrics_port` is set.

## Acceptance testing

Every milestone ships one-click acceptance scripts:
`scripts/accept_m*_local.sh` run the loopback half without root (userspace NFS
clients driving a real server over TCP, byte-verified against the backing tree);
`scripts/accept_m*_vm.sh` run real kernel mounts on a root VM and are wired into
CI (`m1-acceptance` … `m6-acceptance` jobs), including cthon04,
fsx, and pynfs integration. Per-PR CI also runs a six-way build matrix (ASAN, TSAN,
epoll fallback ring, two runner kernel generations), a doc-derived error-whitelist
check (`scripts/gen_errmap_cases.py`), a three-layer benchmark threshold gate
(`scripts/bench_gate.sh`) and a seeded 120 s fuzz run; `nightly.yml` adds the 1 h
fuzz long run, overnight fsx / full pynfs conformance, and a weekly fault-injection
run (`scripts/fault_inject.sh`: kill -9 loops, fsync EIO injection, client kill).

## Status

Phase 6 (M8) item 1 complete — **NFSv4.2 sweets**: `minorversion=2` served on the
unchanged 4.1 session/state machine; SEEK/ALLOCATE/DEALLOCATE (sparse files),
synchronous intra-server COPY (`cp` of large files without the network round
trip) and CLONE (reflink on XFS/Btrfs exports), all stateid-checked and gated by
per-export capability bits probed at startup; the remaining §8 items (second
backend, read delegations, NLM) stay demand-gated. Phase 5 (M7) — **v1 release
candidate**: NFSv4.1 byte-range locks
(gateway LockMgr with POSIX merge/split, LOCK/LOCKT/LOCKU, lock stateids,
non-blocking DENIED with holder info), full SECINFO/SECINFO_NO_NAME, an audited
error whitelist across v3/v4, and the release security hardening — a least-
privilege systemd unit (two capabilities, seccomp allowlist) and a deployment
guide covering the AUTH_SYS trust boundary. Phase 4 (M6, v4.1 read-write + full
state machine), phase 3 (M5, read-only v4.1) and phase 2 (NFSv3 read-write)
remain under regression protection.

Details: [M6 v4.2 sweets notes](docs/m6-v42-sweets.md),
[M5 v4.1 locks + security notes](docs/m5-locks-security.md),
[deployment guide](docs/deployment.md),
[M4 v4.1 read-write notes](docs/m4-v41-readwrite.md),
[M3 v4.1 read-only notes](docs/m3-v41-readonly.md),
[M2 read-write notes](docs/m2-readwrite.md),
[M1 read-only notes](docs/m1-readonly.md); security checklist:
[security-checklist.md](docs/security-checklist.md); backend interface review:
[Backend API v1 review](docs/backend-api-review.md). (These documents are in
Chinese.)
