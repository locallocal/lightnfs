# lightnfs

**A userspace NFS gateway for Linux.** `lightnfsd` serves NFSv3 and NFSv4.1/4.2 on the
north side and talks to pluggable storage backends on the south side — a local POSIX
filesystem, GlusterFS (libgfapi), Lustre and CephFS (libcephfs), all behind one backend
interface that was frozen with the first backend and has taken the three cluster backends
without a change. It is a single process of C++20 coroutines running on an io_uring
reactor (epoll fallback), with no kernel NFS server involved.

[中文文档 / Chinese documentation](docs/README.zh.md)

- Design documents: [docs/design/](docs/design/README.md)
- Protocol research: [docs/nfsv3/](docs/nfsv3/README.md), [docs/nfsv4/](docs/nfsv4/README.md)
- Deployment guide: [docs/deployment.md](docs/deployment.md)

Note: the design and research documents are written in Chinese.

---

## Contents

1. [Why lightnfs](#why-lightnfs)
2. [Feature overview](#feature-overview)
3. [Architecture](#architecture)
4. [Source tree](#source-tree)
5. [Building](#building)
6. [Configuration](#configuration)
7. [Running and mounting](#running-and-mounting)
8. [Administration and observability](#administration-and-observability)
9. [Deployment and security](#deployment-and-security)
10. [Testing](#testing)
11. [Project status and roadmap](#project-status-and-roadmap)
12. [Known limitations](#known-limitations)
13. [Documentation index](#documentation-index)

---

## Why lightnfs

The kernel NFS server (knfsd) is excellent when the data already lives in a local
filesystem the kernel can see. It is much less convenient when the storage is reached
through a user-space library (GlusterFS `libgfapi`), when an NFS front end must be
deployed without root privileges or kernel modules, or when protocol behaviour has to
be auditable and testable in userspace. lightnfs targets exactly that gap:

- **Protocol correctness first.** Every "red line" semantic identified in the protocol
  research — write verifiers, WCC atomicity, persistent file handles, grace-period
  reclaim lists, share reservations, stateid sequencing, courtesy clients — is
  implemented and covered by tests, not approximated.
- **One semantics core for v3 and v4.** The two protocol engines share a single
  protocol-neutral core (exports, handles, permission/squash, per-object serialisation,
  change/WCC sampling). A file looks the same whether mounted with `vers=3` or
  `vers=4.2`, and v3/v4 mixed-mount consistency is covered by the acceptance suite.
- **Pluggable storage boundary.** Backends speak filesystem objects, attributes and
  POSIX errno only; nothing NFS-specific crosses the boundary. Capability bits
  (symlinks, hard links, native change cookies, stable handles, sparse ops, clone,
  copy, byte locks…) make the protocol surface shrink automatically — a backend never
  has to fake a feature it lacks.
- **Fully asynchronous, no thread-per-request.** Network and disk waits never block a
  reactor thread; blocking syscalls without io_uring support run on a bounded offload
  pool. A single process handles 10k+ concurrent connections.
- **Runs unprivileged.** Only `CAP_DAC_READ_SEARCH` (for kernel file handles) and
  `CAP_NET_BIND_SERVICE` (port 2049) are ever needed; both are optional with documented
  degraded modes. The shipped systemd unit runs under a seccomp allowlist.

## Feature overview

### Protocols

| Protocol | Coverage |
|----------|----------|
| **NFSv3** (RFC 1813) | All 21 procedures, read-write, all three WRITE stability levels with COMMIT verifiers, EXCLUSIVE create with restart-persistent verifiers, READDIRPLUS with stable cookies, WCC attributes, a duplicate request cache (DRC) for non-idempotent procedures, per-procedure error whitelist. |
| **MOUNTv3** | MNT/UMNT/EXPORT/DUMP, export-table ACLs by CIDR. |
| **NFSv4.1** (RFC 8881) | Sessions (EXCHANGE_ID/CREATE_SESSION/SEQUENCE with exactly-once slot replay, BIND_CONN_TO_SESSION), pseudo-filesystem root spanning all exports, full open-state machine (OPEN claim NULL/FH/PREVIOUS, share reservations, OPEN_DOWNGRADE, CLOSE), stateid-checked READ/WRITE/COMMIT/SETATTR, namespace ops, byte-range locks (LOCK/LOCKT/LOCKU with POSIX merge/split, non-blocking DENIED with holder info), leases with courtesy clients, grace period with a persistent reclaim list, RECLAIM_COMPLETE, FREE_STATEID/TEST_STATEID, SECINFO/SECINFO_NO_NAME, current-stateid semantics, response-size budgeting (REP_TOO_BIG), UTF-8 name discipline. |
| **NFSv4.2** (RFC 7862) | `minorversion=2` on the unchanged 4.1 state machine: SEEK / ALLOCATE / DEALLOCATE (sparse files), synchronous intra-server COPY (`cp` of large files without the network round trip), CLONE (reflink on XFS/Btrfs exports). Each is advertised per export from a startup capability probe; unimplemented 4.2 ops answer NOTSUPP so clients degrade per operation. |
| **RPC-over-TLS** (RFC 9289) | Optional transport encryption negotiated with the client's AUTH_TLS STARTTLS probe (`xprtsec=tls` on Linux 6.x), TLS session on the same TCP connection, socket I/O still on io_uring (OpenSSL over memory BIOs). `[tls] mode = optional\|required`, server cert/key, optional mutual TLS. "TLS secures the channel; AUTH_SYS declares the identity." Built when OpenSSL is present. |
| **Not served** | NFSv4.0 (`minorversion=0` is rejected; clients fall back to v3 or 4.1), NLM/NSM (v3 locking), pNFS, write/directory delegations, RPCSEC_GSS/Kerberos, UDP. |

### Storage backends

| Backend | Status | Notes |
|---------|--------|-------|
| `local` | shipped | Exports a directory tree. Kernel file handles (`name_to_handle_at`/`open_by_handle_at`) when `CAP_DAC_READ_SEARCH` is available, a path-based fallback otherwise; sharded fd cache with read→write upgrade; sticky fsync-EIO poisoning; `STATX_CHANGE_COOKIE` change attribute on kernels ≥ 6.6; three identity-enforcement modes (`check`, `strict`, `setfsuid`); runtime probe of sparse/copy/clone support. |
| `memory` | test/bench only | Deterministic in-memory tree used by the engine tests and the full-path benchmark. |
| `gluster` | shipped (fake-tested; real-cluster acceptance pending a target environment) | GlusterFS volumes over libgfapi, loaded at runtime (`dlopen`, no build dependency). GFID handles (stable across restarts); storage-side authorization under the caller's identity (`kNativeAccess`); per-object glfd + object-handle caches; `xreaddirplus` cookies; sparse/copy ops; native byte-range locks pushed from the v4 state layer (`kByteLocks`, multi-gateway lock coherence); transport errors surface as JUKEBOX/DELAY so clients retry through brick reconnects. `scripts/accept_gluster.sh` runs the acceptance client against a real volume. |
| `cephfs` | shipped (fake-tested; real-cluster acceptance pending a target environment) | CephFS over libcephfs, loaded at runtime (`dlopen`, no build dependency). The first backend with **both halves of multi-gateway coherence**: the MDS change attribute (`stx_version` → `kNativeChange`, `change_attr_type = MONOTONIC_INCR`) and MDS-arbitrated byte-range locks pushed from the v4 state layer (`kByteLocks`). `vinodeno` handles (inode number + snapid, never reused); a `UserPerm` per call carries the caller's identity; per-object Fh + inode-handle caches; readdirplus cookies with attributes and handles straight from the statx; sparse ops; transport errors surface as JUKEBOX/DELAY, a blocklisted session as a counted hard EIO. `scripts/check_cephapi_abi.sh` verifies the binding against the installed headers; `scripts/accept_cephfs.sh` runs the acceptance client against a real cluster. |
| `lustre` | shipped (fake-tested; real-mount acceptance pending a target environment) | A Lustre client mount, built on the local backend with three seams swapped: FID filehandles (`FILEID_LUSTRE` export handle, opened back through `<mount>/.lustre/fid` — stable across restarts, no `CAP_DAC_READ_SEARCH`), HSM-aware data opens (released files kick `LL_IOC_HSM_REQUEST` RESTORE and answer JUKEBOX/DELAY instead of blocking a worker on the implicit restore), stripe-sized `pref_read`/`pref_write` from the export root's layout, and OFD-lock native byte-range locks (`kByteLocks`, MDS-arbitrated with `-o flock`). Talks to the kernel client with ioctls — no liblustreapi at build or run time (`scripts/check_llapi_abi.sh` verifies the uapi constants where the header exists). `scripts/accept_lustre.sh` runs the acceptance client against a real mount. |

### Operations

- TOML configuration with `--check-config` validation; exports with CIDR client lists,
  `root`/`all`/`none` squash, read-only flag, per-backend sub-table (`[export.local]`,
  `[export.gluster]`, `[export.lustre]`, `[export.cephfs]`).
- `lightnfs-ctl` over a unix socket (ping, metrics, error log dump, DRC and fd-cache
  stats, v4 state table dump, forced client expiry, cluster role status / takeover /
  standby) and `lightnfs-fh` (file-handle decoder with HMAC verification).
- Prometheus text metrics over HTTP, structured logs with per-request summaries.
- Least-privilege systemd unit with a generated seccomp allowlist.

## Architecture

Strictly layered; dependencies point one way only (L1 → L5). The two protocol engines
never depend on each other — all shared semantics live in the core.

```
          NFS clients (kernel or userspace)
                     │  TCP, RPC record marking
 ┌───────────────────▼────────────────────────────────────────┐
 │ L1 transport   listen/accept, framing, connection lifetime, │
 │                backpressure (per-conn inflight, per-peer)   │
 ├────────────────────────────────────────────────────────────┤
 │ L2 rpc         RPC header/xid, AUTH_SYS/AUTH_NONE, program   │
 │                dispatch, DRC (v3), reply encoding           │
 ├──────────────┬─────────────────────┬───────────────────────┤
 │ L3 nfsv3     │ L3 nfsv4 (4.1/4.2)  │ L3 mountd             │
 │ procedures   │ COMPOUND interpreter│ MOUNTv3               │
 ├──────────────┴─────────────────────┴───────────────────────┤
 │ L4 core        export table, file-handle codec (HMAC),      │
 │                permission + squash, per-object locks,       │
 │                pseudo-fs, readdir cursors, error whitelist, │
 │                boot epoch / write verifier                   │
 │     state      StateMgr (clients, sessions, slots, opens,   │
 │                locks, leases, grace) + LockMgr              │
 ├────────────────────────────────────────────────────────────┤
 │ L5 backend     protocol-neutral async object API            │
 │  backend_local │ backend_lustre │ backend_gluster │ backend_cephfs │ memory │
 └────────────────────────────────────────────────────────────┘
   cross-cutting: runtime (coroutines, reactors, io_uring/epoll, offload pool,
                  timers), xdr, util, obs (logs, metrics)
```

**Threading model.** An accept thread assigns each connection round-robin to one of N
reactors (default: one per core). Everything for a connection — parsing, the request
coroutine, the reply — runs on that reactor, so there is no cross-thread
synchronisation inside a connection. File I/O is submitted to the reactor's io_uring;
syscalls without uring support (openat, rename, lseek, fallocate, …) run on a bounded
offload pool and resume the coroutine on its reactor. Cross-connection state (v4 state
tables, DRC, handle cache, per-object locks) is sharded by key with async mutexes.

**Data path of one READ.** `readv` assembles the record → RPC parse → engine decodes
READ → core resolves the handle and checks the export/permissions → backend issues an
io_uring `pread` into a buffer that is later attached to the reply as a zero-copy
segment → `writev`. v4 is the same with a COMPOUND loop around it.

**Crash recovery** relies only on protocol mechanisms (write verifier = boot epoch,
grace period with a persisted reclaim list, courtesy clients), never on a clean
shutdown.

## Source tree

```
src/
  runtime/    coroutine task/spawn, reactors, io_uring and epoll rings, offload pool,
              timers, buffer pool, fake ring for tests
  transport/  listener, connection, record-marked stream, backpressure
  rpc/        call/reply parsing, auth, dispatcher, duplicate request cache
  xdr/        encoder/decoder
  core/       config, export table, file handles, pseudo-fs, readdir, per-object
              locks, error mapping, boot epoch
  state/      v4 StateMgr (clients/sessions/opens/locks/leases/grace), LockMgr
  nfsv3/      v3 engine and wire types
  nfsv4/      v4.1/4.2 engine, attributes, wire types
  mountd/     MOUNTv3
  backend/    backend API, registry and fault-injection hooks; one subdirectory per
              backend: local/, memory/, gluster/ (+ libgfapi binding), lustre/ (FID/HSM
              over the local core, + ioctl uapi binding), cephfs/ (+ libcephfs binding)
  obs/        metrics, error log
  main.cpp    lightnfsd
tools/        lightnfs-ctl (admin CLI + the three-layer benchmarks under
              tools/bench/ with their baseline), lightnfs-fh
tests/        unit/integration tests (mini test framework, fake ring) and the
              userspace acceptance client
fuzz/         nine libFuzzer targets (request path, filehandle decode, TOML config,
              record-stream reassembly, v4 attrs, ObjId parse for the local / gluster /
              lustre / cephfs backends) with checked-in seed corpora (fuzz/seed/) and
              dictionaries (fuzz/dict/); grown corpora stay machine-local under
              fuzz/corpus/
scripts/      ci.sh (one-stop build matrix + gates), fuzz.sh (seeded/nightly runs,
              corpus minimization), one-click acceptance (loopback and VM), pinned
              tool fetchers, bench gate, fault injection, format/tidy/coverage,
              seccomp allowlist generator
packaging/    systemd unit
config/       example configuration
docs/         design, protocol research and deployment docs
```

## Building

Dependencies: CMake ≥ 3.22, Ninja, GCC ≥ 13 or Clang ≥ 17, liburing
(`apt install liburing-dev`, or build the vendored `third_party/liburing` submodule
with `scripts/fetch_liburing.sh` — a built submodule takes precedence over the system
package). Linux ≥ 5.19 recommended (io_uring); any kernel with epoll works through the
fallback ring. The [ccmd](https://github.com/locallocal/ccmd) (command-line entry),
[spdlog](https://github.com/gabime/spdlog) (logging) and
[liburing](https://github.com/axboe/liburing) dependencies are git submodules — clone
with `--recurse-submodules`, or run `git submodule update --init --recursive` in an
existing clone.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build            # ~250 unit/integration cases in lnfs_tests, plus
                                  # per-target fuzz regress and the errmap drift gate
scripts/ci.sh                     # the full matrix: GCC/Clang × Debug/Release ×
                                  # ASAN+UBSAN × TSAN × epoll × fuzz smoke + lint gates
```

Build options:

| Option | Values | Purpose |
|--------|--------|---------|
| `LNFS_SANITIZE` | `address`, `thread` | sanitizer builds (both legs of `scripts/ci.sh`) |
| `LNFS_RING` | `auto` (default), `uring`, `epoll` | default ring backend; `epoll` forces the fallback path |
| `LNFS_BUILD_FUZZ` | `ON` | libFuzzer targets (clang only); driven by `scripts/fuzz.sh` |
| `LNFS_ENABLE_BENCH_GATE` | `ON` | registers `scripts/bench_gate.sh` as a ctest test (Release builds) |

Benchmarks (design 02 §2.8 three layers):

```sh
./build/lightnfs-ctl bench echo     1 4 20000 32 128  # L1: transport echo
./build/lightnfs-ctl bench nullrpc  1 4 20000 32      # L2: null RPC (gate: ≥100k rps single reactor)
./build/lightnfs-ctl bench fullpath 1 4 20000 32 read # L4: full NFS path over the memory backend
scripts/bench_gate.sh                       # all three against tools/bench/baseline.txt
                                            # (usage guide: docs/performance/benchmarks.md)
```

## Configuration

`config/lightnfs.toml.example` is a complete, commented starting point:

```toml
[server]
reactors = 0                 # 0 = one per core
offload_threads = 16
port = 2049
mount_port = 20048
rpcbind = true               # register v3/MOUNT with the local rpcbind
builtin_portmap = false
state_dir = "/var/lib/lightnfs"   # boot epoch, v4 reclaim list, ctl socket
max_connections = 4096
per_peer_limit = 128
max_request_size = "2MiB"
metrics_port = 0             # Prometheus text endpoint; 0 = disabled

[protocol]
v3 = true
v4 = true                    # minorversion 1 and 2
lease = "90s"                # v4.1 lease; also the grace window after a restart
courtesy_multiplier = 24     # expired clients are kept up to 24 × lease
drc_ttl = "120s"
drc_mem = "64MiB"

[limits]
inflight_per_conn = 64

[[export]]
path = "/srv/data"
backend = "local"
fsid = 1
clients = ["192.168.0.0/24", "127.0.0.0/8"]
squash = "root"              # root | all | none
readonly = false

[export.local]
handles = "auto"             # auto | kernel | fallback
fd_cache = 4096
readdir_enrich = true
identity = "check"           # check | strict | setfsuid (root only)
```

Validate with `lightnfsd --check-config --config FILE`. Multiple `[[export]]` blocks are
allowed; v4 clients see them all under one pseudo root. See design 08 for every key.

## Running and mounting

```sh
cp config/lightnfs.toml.example /tmp/lightnfs.toml   # adjust the export path
./build/lightnfsd --check-config --config /tmp/lightnfs.toml
./build/lightnfsd --config /tmp/lightnfs.toml
```

Mounting from a Linux client:

```sh
# NFSv3 (through rpcbind, or with explicit ports when rpcbind = false)
mount -t nfs -o vers=3 server:/srv/data /mnt
mount -t nfs -o vers=3,port=2049,mountport=20048 server:/srv/data /mnt

# NFSv4.1 — exports appear under the pseudo root
mount -t nfs -o vers=4.1 server:/srv/data /mnt

# NFSv4.2 — adds sparse-file ops, server-side copy and reflink
mount -t nfs -o vers=4.2 server:/srv/data /mnt
fallocate -p -o 1M -l 1M /mnt/big      # DEALLOCATE
cp /mnt/big /mnt/copy                  # COPY (no client round trip)
cp --reflink=auto /mnt/big /mnt/clone  # CLONE on XFS/Btrfs exports, COPY otherwise
```

At startup the server logs, per export, which v4.2 capabilities the backend filesystem
supports (`export /srv/data v4.2 capabilities: seek/allocate=true copy=true clone=false`).

## Administration and observability

```sh
lightnfs-ctl ping
lightnfs-ctl metrics                 # same text as the Prometheus endpoint
lightnfs-ctl dump-errors             # recent error log ring
lightnfs-ctl drc                     # duplicate-request-cache stats
lightnfs-ctl fdcache                 # fd / handle-cache stats (local, gluster, cephfs exports)
lightnfs-ctl state                   # v4 clients/sessions/opens/locks, grace status
lightnfs-ctl expire-client 0x500000001   # force-reclaim one client's state
lightnfs-ctl cluster status          # multi-gateway role / epoch / fence owner / peers
lightnfs-ctl cluster takeover        # standby gateway takes the fence (--force: take a live one)
lightnfs-ctl cluster standby         # active gateway drains and releases the fence
lightnfs-fh --key /etc/lightnfs/hmac.key <hex-handle>   # decode + verify a handle
```

The ctl socket defaults to `<state_dir>/ctl.sock` (`LIGHTNFS_CTL` overrides the path).
Metrics cover per-procedure call/error counters, bytes read/written, DRC, fd cache, v4
state counts (clients, sessions, opens, courtesy, grace), reclaim reasons, and
connection limits. Logs are structured (`ts= level= …`) with a per-request summary
line available at debug level.

## Deployment and security

Read [docs/deployment.md](docs/deployment.md) before exposing the server. The short
version:

- **Trust boundary: AUTH_SYS only.** The client's uid/gid are trusted as presented;
  run on a trusted network. Enable built-in **RPC-over-TLS** (`[tls]`, RFC 9289) or
  front with WireGuard for channel encryption, and use the CIDR client lists and squash
  modes on every export. TLS secures the channel but AUTH_SYS still declares identity —
  the trusted-network assumption stands.
- **Least privilege.** `packaging/systemd/lightnfs.service` runs as a dedicated user
  with `CAP_DAC_READ_SEARCH` + `CAP_NET_BIND_SERVICE`, `ProtectSystem=strict`, and a
  seccomp allowlist generated from a real workload (`scripts/gen_seccomp_allowlist.sh`).
- **Handles.** File handles are HMAC-authenticated; kernel handles are stable across
  restarts, the fallback mode is documented with its limits.
- **Restart behaviour.** Crash or restart → new write verifier (clients resend
  uncommitted data) and a grace period sized by `lease` during which listed v4 clients
  reclaim their opens/locks.

The release checklist with per-item verification is in
[docs/security-checklist.md](docs/security-checklist.md).

## Testing

Test layers:

| Layer | How |
|-------|-----|
| Runtime | fake-ring timing tests, ASAN + TSAN builds, EINTR/short-read injection |
| XDR / request path | round-trip tests; libFuzzer over nine parse boundaries — `scripts/fuzz.sh smoke` (120 s seeded, per change, also part of `ci.sh`) and `scripts/fuzz.sh nightly` (1 h); seeds are checked in, grown corpora stay machine-local (`fuzz.sh minimize` compacts them) |
| Backend contract | handle stability, 100k-entry cookie stability, write stability levels + sticky fsync EIO, v4.2 sparse/copy/clone |
| Error mapping | the v3 whitelist test is **generated from the research document** (`scripts/gen_errmap_cases.py`); doc/code drift fails ctest (`errmap_check`); v4 rows checked against RFC tables |
| Lint / coverage | `scripts/format_check.sh` (clang-format gate, part of `ci.sh`), `scripts/tidy.sh` (clang-tidy, bugprone/performance/concurrency), `scripts/coverage.sh` (llvm-cov report) |
| Performance | three-layer benchmark gate against `tools/bench/baseline.txt` |
| Conformance | cthon04, fsx, pynfs (4.1) on real kernel mounts via `accept_m*_vm.sh`; overnight fsx and the full pynfs suite as soak runs |
| v3/v4 consistency | dual-mount comparisons and mixed-version writes |
| Fault injection | weekly: kill -9 restart loops, fsync EIO injection, client kill, v4 restart reclaim (`scripts/fault_inject.sh`) |

Acceptance is fully scripted, one pair per protocol generation:
`scripts/accept_m2_local.sh` (NFSv3 read-write) and `scripts/accept_m6_local.sh`
(NFSv4.1/4.2 with locks, reclaim, courtesy) run without root — a userspace NFS client
drives a real server over loopback, byte-verified against the backing tree, Release +
ASAN — while the matching `accept_m2_vm.sh`/`accept_m6_vm.sh` run real kernel mounts
on a root machine. `scripts/ci.sh` is the one-stop driver: the build-matrix sweep
(GCC/Clang, ASAN, TSAN, epoll ring, fuzz regress) plus the errmap and format gates as
the per-change run, and `ci.sh nightly` adds the bench floor gate and a 1 h fuzz run.
The long soaks (24h fuzz via `fuzz.sh`, overnight fsx, weekly `fault_inject.sh`) are
invocable locally or from any scheduler of your choice; the repository does not carry
hosted CI pipelines. External suites are fetched at pinned commits
(`scripts/fetch_{cthon,fsx,pynfs}.sh`; `LNFS_FETCH_HEAD=1` bumps a pin).

## Project status and roadmap

Development proceeded in phases, each closed by an acceptance run:

| Phase | Milestone | Content |
|-------|-----------|---------|
| 0 | runtime | coroutine runtime, io_uring/epoll, XDR, transport, benchmarks |
| 1 | M1 | Backend API v1 frozen, NFSv3 read-only, MOUNTv3 |
| 2 | M2–M3 | NFSv3 read-write, DRC, fd cache, observability, first security items |
| 3 | M5 | NFSv4.1 read-only: sessions, pseudo-fs, client/session state |
| 4 | M6 | NFSv4.1 read-write: full open-state machine, leases, courtesy, grace/reclaim |
| 5 | M7 | byte-range locks, SECINFO, error-whitelist audit, security hardening — **v1 release candidate** |
| 6 | M8 | **done:** NFSv4.2 SEEK/ALLOCATE/DEALLOCATE, COPY, CLONE; READ_PLUS; read delegations + callback channel; GlusterFS, Lustre and CephFS backends with native lock push-down; test infrastructure (fuzzing, fault injection, CI matrix). **Demand-gated:** NLM/NSM |

Longer-term observations: **RPC-over-TLS (RFC 9289) is now implemented** (`[tls]`).
Still not committed — write/directory delegations (need CB_GETATTR and single-writer
workload evidence) and pNFS flexfiles MDS (a deliberate non-goal, decision D8).
Multi-gateway consistency now has a backend with both prerequisites (CephFS: native
change attribute + MDS-arbitrated locks), but the real multi-gateway validation is
pending a target cluster and v4 open/deny state stays per gateway.

## Known limitations

- AUTH_SYS only for **identity**; no Kerberos/RPCSEC_GSS. Channel encryption is available
  via built-in RPC-over-TLS (`[tls]`, RFC 9289) or an external tunnel.
- No NLM/NSM: v3 clients have no byte-range locks, and v3 writes are not constrained
  by v4 share reservations or locks on the same export (documented boundary).
- Read delegations only (no write/directory delegations), no pNFS, no asynchronous or
  inter-server COPY, no xattr (the ops answer NOTSUPP).
- Single gateway: v4 state lives in process memory plus the on-disk reclaim list; no
  state sharing between gateways.
- Handle stability in `handles = "auto"` fallback mode depends on the filesystem
  (documented; use kernel handles with `CAP_DAC_READ_SEARCH` in production).

## Documentation index

- Design: [docs/design/](docs/design/README.md) — architecture, runtime/concurrency,
  transport/RPC/XDR, NFS core, backend API, backend implementations, state management,
  configuration/observability, multi-gateway failover design (proposal)
- Protocol research: [docs/nfsv3/](docs/nfsv3/README.md), [docs/nfsv4/](docs/nfsv4/README.md)
- Operations: [deployment guide](docs/deployment.md),
  [security checklist](docs/security-checklist.md)
- Performance: [benchmark tool guide](docs/performance/benchmarks.md) (`lightnfs-ctl bench`,
  `scripts/bench_gate.sh`, baseline policy, real-mount fio guidance) and the
  [test report](docs/performance/test-report.md) — conformance runs (cthon / pynfs / fsx),
  benchmark numbers against `tools/bench/baseline.txt`, unit/fuzz/backend test coverage
