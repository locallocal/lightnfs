# lightnfs

**Linux 用户态 NFS 网关。** `lightnfsd` 北向同时服务 NFSv3 与 NFSv4.1/4.2，南向对接可插拔
存储后端（v1 交付本地 POSIX 文件系统后端；接口已按 Lustre / GlusterFS 映射表评审）。
单进程 C++20 协程全异步，运行在 io_uring reactor 之上（epoll 兜底），不依赖内核 NFS 服务。

[English README](../README.md)

- 设计文档：[design/](design/README.md)
- 协议调研：[nfsv3/](nfsv3/README.md)、[nfsv4/](nfsv4/README.md)
- 部署指南：[deployment.md](deployment.md)

---

## 目录

1. [为什么做 lightnfs](#为什么做-lightnfs)
2. [功能总览](#功能总览)
3. [架构](#架构)
4. [源码结构](#源码结构)
5. [构建](#构建)
6. [配置](#配置)
7. [运行与挂载](#运行与挂载)
8. [管理与可观测性](#管理与可观测性)
9. [部署与安全](#部署与安全)
10. [测试](#测试)
11. [项目状态与路线](#项目状态与路线)
12. [已知限制](#已知限制)
13. [文档索引](#文档索引)

---

## 为什么做 lightnfs

内核 NFS 服务（knfsd）在"数据就在内核可见的本地文件系统上"时非常好用；但当存储只能
通过用户态库访问（GlusterFS `libgfapi`）、需要在无 root / 无内核模块的环境部署 NFS
前端、或者协议行为必须在用户态可审计可测试时，它就不合适了。lightnfs 正对准这个缺口：

- **协议正确性优先。** 调研分册里标注为"红线"的语义——写验证器、WCC 原子性、句柄
  持久性、宽限期 reclaim 名单、share 预留、stateid 序号、courtesy 客户端——全部实现并
  有测试覆盖，不做近似。
- **v3 与 v4 共用一个语义核心。** 两个协议引擎共享协议无关的 core（导出表、句柄、权限
  与 squash、per-object 串行化、change/WCC 采样）。同一文件用 `vers=3` 与 `vers=4.2` 挂载
  行为一致，v3/v4 混合挂载对比属验收套件常规项。
- **可插拔的存储边界。** 后端只谈文件系统对象、属性与 POSIX errno，任何 NFS 概念都不
  跨越边界。能力位（符号链接、硬链接、原生 change、稳定句柄、稀疏操作、clone、copy、
  字节锁……）让协议表面自动收缩——后端永远不需要"假装支持"。
- **全异步，无每请求线程。** 网络与磁盘等待从不阻塞 reactor 线程；无 io_uring 支持的
  阻塞系统调用走有界 offload 池。单进程可承载万级并发连接。
- **可无特权运行。** 最多只需 `CAP_DAC_READ_SEARCH`（内核句柄）与 `CAP_NET_BIND_SERVICE`
  （2049 端口），二者皆可选并有文档化的降级模式；随附的 systemd 单元在 seccomp 白名单下
  运行。

## 功能总览

### 协议

| 协议 | 覆盖 |
|------|------|
| **NFSv3**（RFC 1813） | 全部 21 个过程，读写，WRITE 三种稳定级 + COMMIT 验证器，EXCLUSIVE create 的跨重启验证器，READDIRPLUS 稳定 cookie，WCC 属性，非幂等过程的重复请求缓存（DRC），逐过程错误白名单。 |
| **MOUNTv3** | MNT/UMNT/EXPORT/DUMP，导出表 CIDR 访问控制。 |
| **NFSv4.1**（RFC 8881） | 会话（EXCHANGE_ID/CREATE_SESSION/SEQUENCE 槽位 exactly-once 重放、BIND_CONN_TO_SESSION），横跨所有导出的伪根，完整 open 状态机（OPEN claim NULL/FH/PREVIOUS、share 预留、OPEN_DOWNGRADE、CLOSE），stateid 校验的 READ/WRITE/COMMIT/SETATTR，命名空间操作，字节区间锁（LOCK/LOCKT/LOCKU，POSIX 合并/拆分，非阻塞 DENIED 带持有者），租约与 courtesy 客户端，带持久 reclaim 名单的宽限期，RECLAIM_COMPLETE，FREE_STATEID/TEST_STATEID，SECINFO/SECINFO_NO_NAME，current stateid 语义，应答大小预算（REP_TOO_BIG），UTF-8 名字纪律。 |
| **NFSv4.2**（RFC 7862） | 在不变的 4.1 状态机上宣告 `minorversion=2`：SEEK / ALLOCATE / DEALLOCATE（稀疏文件）、同步同服 COPY（大文件 `cp` 不再绕经客户端）、CLONE（XFS/Btrfs 导出上的 reflink）。每项由启动时的能力探测按导出宣告；未实现的 4.2 操作回 NOTSUPP，客户端逐操作降级。 |
| **RPC-over-TLS**（RFC 9289） | 可选的传输层加密：客户端以 AUTH_TLS 探测 NULL 过程触发 STARTTLS（Linux 6.x 的 `xprtsec=tls`），在同一 TCP 连接上握手，其后 RPC 走 TLS 会话；socket IO 仍全走 io_uring（OpenSSL 在 memory BIO 上做密码学）。`[tls] mode = optional\|required`、服务器证书/私钥、可选双向 TLS。"TLS 保通道、AUTH_SYS 报身份"。构建时有 OpenSSL 即启用。 |
| **不提供** | NFSv4.0（`minorversion=0` 拒绝，客户端回退 v3 或 4.1）、NLM/NSM（v3 锁）、pNFS、写/目录委托、RPCSEC_GSS/Kerberos、UDP。 |

### 存储后端

| 后端 | 状态 | 说明 |
|------|------|------|
| `local` | 已交付 | 导出一棵目录树。有 `CAP_DAC_READ_SEARCH` 时用内核句柄（`name_to_handle_at`/`open_by_handle_at`），否则走路径兜底；分片 fd 缓存、读→写升级；粘性 fsync-EIO；内核 ≥ 6.6 用 `STATX_CHANGE_COOKIE` 作 change 属性；三种身份执行模式（`check`/`strict`/`setfsuid`）；运行时探测稀疏/拷贝/clone 支持。 |
| `memory` | 仅测试/基准 | 确定性内存树，供引擎测试与全链路基准使用。 |
| Lustre / GlusterFS | 接口已验证，未实现 | Backend API v1 按两者的映射表评审通过（设计 06）；实现等待目标环境。 |

### 运维

- TOML 配置，`--check-config` 校验；导出支持 CIDR 客户端列表、`root`/`all`/`none`
  squash、只读标志、后端子表。
- `lightnfs-ctl`（unix socket：ping、metrics、错误日志、DRC 与 fd 缓存统计、v4 状态表、
  强制客户端过期）与 `lightnfs-fh`（句柄解码 + HMAC 校验）。
- HTTP Prometheus 文本指标，结构化日志，每请求摘要。
- 最小特权 systemd 单元 + 生成式 seccomp 白名单。

## 架构

严格分层，依赖只向下（L1 → L5）。两个协议引擎互不依赖，所有共享语义放在 core。

```
          NFS 客户端（内核或用户态）
                     │  TCP，RPC 记录标记
 ┌───────────────────▼────────────────────────────────────────┐
 │ L1 transport   listen/accept、帧、连接生命周期、背压          │
 │                （每连接在途上限、每对端连接上限）              │
 ├────────────────────────────────────────────────────────────┤
 │ L2 rpc         RPC 头/xid、AUTH_SYS/AUTH_NONE、程序分发、     │
 │                DRC（v3）、应答编码                             │
 ├──────────────┬─────────────────────┬───────────────────────┤
 │ L3 nfsv3     │ L3 nfsv4（4.1/4.2） │ L3 mountd             │
 │ 过程         │ COMPOUND 解释器      │ MOUNTv3               │
 ├──────────────┴─────────────────────┴───────────────────────┤
 │ L4 core        导出表、句柄编解码（HMAC）、权限 + squash、     │
 │                per-object 锁、伪根、readdir 游标、错误白名单、 │
 │                boot epoch / 写验证器                          │
 │     state      StateMgr（客户端、会话、槽位、open、锁、租约、  │
 │                宽限期）+ LockMgr                              │
 ├────────────────────────────────────────────────────────────┤
 │ L5 backend     协议无关的异步对象接口                          │
 │                backend_local  │  backend_memory  │  （…）    │
 └────────────────────────────────────────────────────────────┘
   横切：runtime（协程、reactor、io_uring/epoll、offload 池、定时器）、xdr、util、
         obs（日志、指标）
```

**线程模型。** accept 线程把连接轮转指派给 N 个 reactor（默认每核一个）。一条连接的
解析、请求协程、应答全部在同一 reactor 上执行，连接内无跨线程同步。文件 IO 提交到
reactor 的 io_uring；无 uring 支持的系统调用（openat、rename、lseek、fallocate……）在
有界 offload 池执行并回到原 reactor 恢复协程。跨连接状态（v4 状态表、DRC、句柄缓存、
per-object 锁）按 key 分片 + 异步互斥。

**一次 READ 的数据流。** `readv` 拼出完整记录 → RPC 解析 → 引擎解码 READ → core 解析
句柄并检查导出/权限 → 后端发起 io_uring `pread` 到一块之后作为零拷贝段挂进应答的缓冲
→ `writev`。v4 同构，外面多一层 COMPOUND 循环。

**崩溃恢复** 只依赖协议机制（写验证器 = boot epoch、带持久 reclaim 名单的宽限期、
courtesy 客户端），不依赖干净退出。

## 源码结构

```
src/
  runtime/    协程 task/spawn、reactor、io_uring 与 epoll ring、offload 池、定时器、
              缓冲池、测试用 fake ring
  transport/  监听、连接、记录标记流、背压
  rpc/        call/reply 解析、认证、分发、重复请求缓存
  xdr/        编解码
  core/       配置、导出表、文件句柄、伪根、readdir、per-object 锁、错误映射、boot epoch
  state/      v4 StateMgr（客户端/会话/open/锁/租约/宽限期）、LockMgr
  nfsv3/      v3 引擎与线上类型
  nfsv4/      v4.1/4.2 引擎、属性、线上类型
  mountd/     MOUNTv3
  backend/    后端接口、local 与 memory 后端
  obs/        指标、错误日志
  main.cpp    lightnfsd
tools/        lightnfs-ctl（管理 CLI + tools/bench/ 下的三层基准与基线）、lightnfs-fh
tests/        单测/集成测试（迷你测试框架、fake ring）与用户态验收客户端
fuzz/         覆盖完整请求路径的 libFuzzer 入口 + 入库语料
scripts/      一键验收（回环与 VM）、数据集/工具获取、基准门禁、故障注入、seccomp 白名单生成
packaging/    systemd 单元
config/       配置样例
docs/         设计、协议调研与部署文档
```

## 构建

依赖：CMake ≥ 3.22、Ninja、GCC ≥ 13 或 Clang ≥ 17、liburing（`apt install liburing-dev`，
或运行 `scripts/fetch_liburing.sh` 构建 `third_party/liburing` 子模块——已构建的子模块
优先于系统包）。推荐 Linux ≥ 5.19（io_uring）；任何有 epoll 的内核都能通过兜底 ring 运行。
[ccmd](https://github.com/locallocal/ccmd)（命令行入口）、
[spdlog](https://github.com/gabime/spdlog)（日志）与
[liburing](https://github.com/axboe/liburing) 依赖均为 git 子模块——克隆时加
`--recurse-submodules`，已有克隆运行 `git submodule update --init --recursive`。

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build            # 单测 + 集成测试（约 120 项，< 1 分钟）
```

构建选项：

| 选项 | 取值 | 用途 |
|------|------|------|
| `LNFS_SANITIZE` | `address`、`thread` | sanitizer 构建（常规矩阵两者都跑） |
| `LNFS_RING` | `auto`（默认）、`uring`、`epoll` | 默认 ring 后端；`epoll` 强制兜底路径 |
| `LNFS_BUILD_FUZZ` | `ON` | libFuzzer 目标（仅 clang）：`./build/fuzz_handle_request fuzz/corpus` |

基准（设计 02 §2.8 三层）：

```sh
./build/lightnfs-ctl bench echo     1 4 20000 32 128  # L1：传输层 echo
./build/lightnfs-ctl bench nullrpc  1 4 20000 32      # L2：null RPC（门禁：单 reactor ≥100k rps）
./build/lightnfs-ctl bench fullpath 1 4 20000 32 read # L4：全链路（memory 后端）
scripts/bench_gate.sh                       # 三项对照 tools/bench/baseline.txt
```

## 配置

`config/lightnfs.toml.example` 是完整、带注释的起点：

```toml
[server]
reactors = 0                 # 0 = 每核一个
offload_threads = 16
port = 2049
mount_port = 20048
rpcbind = true               # 向本机 rpcbind 注册 v3/MOUNT
builtin_portmap = false
state_dir = "/var/lib/lightnfs"   # boot epoch、v4 reclaim 名单、ctl socket
max_connections = 4096
per_peer_limit = 128
max_request_size = "2MiB"
metrics_port = 0             # Prometheus 文本端点；0 = 关闭

[protocol]
v3 = true
v4 = true                    # minorversion 1 与 2
lease = "90s"                # v4.1 租约；也是重启后的宽限期
courtesy_multiplier = 24     # 过期客户端最长保留 24 × lease
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
identity = "check"           # check | strict | setfsuid（需 root）
```

用 `lightnfsd --check-config --config FILE` 校验。可写多个 `[[export]]`，v4 客户端在一个
伪根下看到全部导出。全部键的说明见设计 08。

## 运行与挂载

```sh
cp config/lightnfs.toml.example /tmp/lightnfs.toml   # 按需修改导出路径
./build/lightnfsd --check-config --config /tmp/lightnfs.toml
./build/lightnfsd --config /tmp/lightnfs.toml
```

Linux 客户端挂载：

```sh
# NFSv3（经 rpcbind；rpcbind = false 时显式指定端口）
mount -t nfs -o vers=3 server:/srv/data /mnt
mount -t nfs -o vers=3,port=2049,mountport=20048 server:/srv/data /mnt

# NFSv4.1 —— 导出出现在伪根下
mount -t nfs -o vers=4.1 server:/srv/data /mnt

# NFSv4.2 —— 增加稀疏文件操作、服务器端拷贝与 reflink
mount -t nfs -o vers=4.2 server:/srv/data /mnt
fallocate -p -o 1M -l 1M /mnt/big      # DEALLOCATE
cp /mnt/big /mnt/copy                  # COPY（不经客户端往返）
cp --reflink=auto /mnt/big /mnt/clone  # XFS/Btrfs 导出上 CLONE，否则退到 COPY
```

启动时服务器按导出记录后端文件系统支持的 v4.2 能力
（`export /srv/data v4.2 capabilities: seek/allocate=true copy=true clone=false`）。

## 管理与可观测性

```sh
lightnfs-ctl ping
lightnfs-ctl metrics                 # 与 Prometheus 端点相同的文本
lightnfs-ctl dump-errors             # 最近错误日志环
lightnfs-ctl drc                     # 重复请求缓存统计
lightnfs-ctl fdcache                 # local 后端 fd 缓存统计
lightnfs-ctl state                   # v4 客户端/会话/open/锁、宽限期状态
lightnfs-ctl expire-client 0x500000001   # 强制回收某客户端状态
lightnfs-fh --key /etc/lightnfs/hmac.key <hex-handle>   # 解码并校验句柄
```

ctl socket 默认 `<state_dir>/ctl.sock`（`LIGHTNFS_CTL` 覆盖路径）。指标覆盖逐过程
调用/错误计数、读写字节、DRC、fd 缓存、v4 状态计数（客户端、会话、open、courtesy、
宽限期）、回收原因、连接限制。日志为结构化格式（`ts= level= …`），debug 级别提供每请求
摘要行。

## 部署与安全

对外暴露前请先读 [deployment.md](deployment.md)。要点：

- **信任边界：仅 AUTH_SYS。** 客户端声明的 uid/gid 被直接信任；只在受信网络运行。
  启用内置 **RPC-over-TLS**（`[tls]`，RFC 9289）或前置 WireGuard 做通道加密，每个导出都用
  CIDR 客户端列表与 squash。TLS 保通道但 AUTH_SYS 仍报身份——受信网络假设不变。
- **最小特权。** `packaging/systemd/lightnfs.service` 以专用用户运行，仅
  `CAP_DAC_READ_SEARCH` + `CAP_NET_BIND_SERVICE`，`ProtectSystem=strict`，seccomp 白名单由
  真实负载生成（`scripts/gen_seccomp_allowlist.sh`）。
- **句柄。** 文件句柄带 HMAC 认证；内核句柄跨重启稳定，兜底模式的限制有文档说明。
- **重启行为。** 崩溃或重启 → 新的写验证器（客户端重发未提交数据）+ 以 `lease` 为长度
  的宽限期，名单内的 v4 客户端在其中 reclaim open/锁。

逐项验证记录见 [security-checklist.md](security-checklist.md)。

## 测试

测试层次（开发计划 §9）：

| 层 | 手段 |
|----|------|
| 运行时 | fake ring 时序测试、ASAN + TSAN 构建、EINTR/短读注入 |
| XDR / 请求路径 | round-trip 测试；覆盖完整请求路径的 libFuzzer（每 PR 120s 种子跑，每日 1h 长跑并增长语料） |
| 后端契约 | 句柄稳定性、10 万项 cookie 稳定性、写稳定级 + 粘性 fsync EIO、v4.2 稀疏/拷贝/clone |
| 错误映射 | v3 白名单测试**由调研文档生成**（`scripts/gen_errmap_cases.py`），文档/代码偏差即测试失败；v4 行对照 RFC 表 |
| 性能 | 三层基准对照 `tools/bench/baseline.txt` 的门禁 |
| 一致性 | cthon04、fsx、pynfs（4.1）经 `accept_m*_vm.sh` 在真实内核挂载上运行；过夜 fsx 与 pynfs 全量为长稳项 |
| v3/v4 一致 | 双挂载对比与混合版本写 |
| 故障注入 | 每周：kill -9 重启循环、fsync EIO 注入、客户端消失、v4 带状态重启 reclaim（`scripts/fault_inject.sh`） |

验收全部脚本化，按协议代际各留一对：`scripts/accept_m2_local.sh`（NFSv3 读写）与
`scripts/accept_m6_local.sh`（NFSv4.1/4.2 含锁、reclaim、courtesy）无 root——用户态
NFS 客户端经回环驱动真实服务器，与后备目录树逐字节对照，Release + ASAN；对应的
`accept_m2_vm.sh`/`accept_m6_vm.sh` 在 root 机器上做真实内核挂载。全部测试都由这些脚本驱动——构建矩阵（GCC/Clang、ASAN、
TSAN、epoll ring）、逐次改动回归与长稳任务（24h fuzz、fsx 过夜、每周故障注入）均可
本地或用任意调度器按需执行；仓库不携带托管 CI 流水线。

## 项目状态与路线

按 [design/09-roadmap.md](design/09-roadmap.md) 的路线分阶段推进，每阶段以验收收尾：

| 阶段 | 里程碑 | 内容 |
|------|--------|------|
| 0 | 运行时 | 协程运行时、io_uring/epoll、XDR、传输层、基准 |
| 1 | M1 | Backend API v1 定稿、NFSv3 只读、MOUNTv3 |
| 2 | M2–M3 | NFSv3 读写、DRC、fd 缓存、可观测性、首批安全项 |
| 3 | M5 | NFSv4.1 只读：会话、伪根、客户端/会话状态 |
| 4 | M6 | NFSv4.1 读写：open 状态机全量、租约、courtesy、宽限期/reclaim |
| 5 | M7 | 字节区间锁、SECINFO、错误白名单复查、安全加固——**v1 发布候选** |
| 6 | M8 | **已完成：** NFSv4.2 SEEK/ALLOCATE/DEALLOCATE、COPY、CLONE；测试基建。**按需触发：** 第二后端（Lustre/GlusterFS）、读委托 + 回传通道、NLM/NSM |

长期观察项：**RPC-over-TLS（RFC 9289）已实现**（`[tls]`）。仍不承诺——写/目录委托
（需 CB_GETATTR 与单写者工作负载证据）、pNFS flexfiles MDS（决策 D8 的明确非目标）、
基于原生 change 与锁的多网关一致性（需下沉原生锁的集群后端）。

## 已知限制

- 身份仅 AUTH_SYS；无 Kerberos/RPCSEC_GSS。通道加密可用内置 RPC-over-TLS（`[tls]`，
  RFC 9289）或外部隧道。
- 无 NLM/NSM：v3 客户端没有字节锁，同一导出上 v3 写不受 v4 share 预留或锁约束
  （文档化边界）。
- 无写/目录委托、无 pNFS、无异步或跨服 COPY、无 READ_PLUS/xattr。
- 单网关：v4 状态在进程内存 + 磁盘 reclaim 名单；网关间不共享状态。
- `handles = "auto"` 兜底模式下句柄稳定性取决于文件系统（已文档化；生产用
  `CAP_DAC_READ_SEARCH` + 内核句柄）。

## 文档索引

- 设计：[design/](design/README.md)——架构、运行时/并发、传输/RPC/XDR、NFS 核心、
  后端接口、后端映射、状态管理、配置/可观测性、路线图
- 协议调研：[nfsv3/](nfsv3/README.md)、[nfsv4/](nfsv4/README.md)
- 运维：[部署指南](deployment.md)、[安全清单](security-checklist.md)
