# 10. 优化与功能规划（全量代码审查，2026-08）

本册是对 v1 完成后（M8 收尾）整个代码库的一次全量审查结论：先修的正确性/安全问题、
高性价比的性能优化、可观测性与运维缺口、下一阶段值得规划的功能，以及结构性重构与
测试工程化。与 [09-roadmap](09-roadmap.md) 的关系：09 册记录 v1 的里程碑历史与
长期观察项，本册给出 v1.x → v2 的具体工作清单，每项均附代码证据（文件:行号）。

审查范围：`src/` 全部子系统、`tools/`、`tests/`、`fuzz/`、`scripts/`、设计文档与实现
的一致性。全仓库显式 TODO 仅 1 条（`src/runtime/io.hpp:97`，with_timeout 不取消败方
定时器），因此本册同时承担"隐性待办登记"的职责。

## 目录

1. [正确性与安全问题（先修）](#1-正确性与安全问题先修)
2. [性能优化](#2-性能优化)
3. [可观测性补齐](#3-可观测性补齐)
4. [运维能力规划](#4-运维能力规划)
5. [协议功能规划](#5-协议功能规划)
6. [结构性重构](#6-结构性重构)
7. [测试与工程化](#7-测试与工程化)
8. [分阶段执行建议](#8-分阶段执行建议)

---

## 1. 正确性与安全问题（先修）

这些不是优化，是缺陷。建议全部在下一个 patch 版本内修复，且每项补一个针对性测试。

### 1.1 P0：local 后端配置接线 bug——`identity` / `readdir_enrich` 被静默忽略 ✅ 已修复

`core/config.cpp:352-362` 对 `backend == "local"` 特判、绕过后端工厂注册表，只读
`fd_cache` 与 `handles` 两个键；而完整解析 `identity`、`readdir_enrich` 的
`make_local()`（`backend/local.cpp:1222-1240`）只有走 `find_backend()` 才会执行。
后果：`config/lightnfs.toml.example` 里示范的 `identity = "strict"/"setfsuid"` 与
`readdir_enrich` **在 lightnfsd 里完全不生效**，设计 06 §6.4 的身份执行模式
（`local.cpp:602-673` 已实现）在生产路径不可达。

- 修复：删掉特判，统一走 `find_backend("local")`；顺带给两处裸 `std::stoull`
  （`config.cpp:355`、`local.cpp:1227`）加错误处理——目前 `fd_cache = "abc"` 会抛
  异常穿透 `ExportTable::build`。
- 回归：加一个"配置键端到端生效"的测试，防止再次出现声明了但接不上的键。

### 1.2 P0：lock stateid 检查中的数据竞争

`state/state_mgr.cpp:824` 在 state 分片锁已释放（:810 作用域结束）后读取
`open.bopen`（父 open state 的 `shared_ptr<backend::Object>`），而并发 CLOSE 走
`unlink_state()`（`state_mgr.cpp:586`）在**另一个分片**的锁下对同一成员做
`std::move`。"持锁拷贝"的自述不变式（`state_mgr.hpp:96-99`）被违反，
拷贝 vs 移动构成 UB。用 lock stateid 做 READ/WRITE 与并发 CLOSE 父 open 即可触发。

- 修复：按父 open 的 `other` 重新加对应分片锁后再取 `bopen`（保持"一次一锁"纪律）。
- 回归：加针对性并发测试并纳入 TSAN 构建。

### 1.3 P0：fd cache 无硬上限，可耗尽进程 fd

`backend/local.cpp:127-137` 的淘汰循环在"全片条目都在使用中"时 `break`，缓存越过
容量继续增长；高并发下 fd 数可突破 RLIMIT_NOFILE，且没有"超容量"计数暴露。
另外淘汰本身是持分片锁的 O(N) 全表扫描（默认每片 256 条），插入路径 O(N²)。

- 修复：改为侵入式 LRU 链表（同时消掉线性扫描），全在用时至少计数 + 告警；
  参见 §2.4 的整体 fd cache 改造。

### 1.4 P0：io_uring 提交路径的两个健壮性缺陷

- `io_uring_submit()` 返回值三处全部被丢弃（`runtime/uring_ring.cpp:57、69、141`）。
  CQ overflow 时 submit 返回 `-EBUSY`，SQE 静默不提交，等待的协程**永久挂起**且无日志。
- `get_sqe()`（`uring_ring.cpp:66-74`）在 SQ 耗尽时靠 `assert` 兜底，`NDEBUG` 下
  返回 nullptr 直接被 `io_uring_prep_*` 解引用。

### 1.5 P1：状态资源无上限（DoS 面）

`NFS4ERR_RESOURCE` 已定义（`nfsv4/nfs4_types.hpp:131`）但从未被返回；没有
max clients / max opens per client / max lock segments 任何限制。失控客户端可无限
创建 open/lock state 撑爆内存。配套的还有两处无界增长：

- `lock_owners_` 表只插不删（`state_mgr.hpp:352-353`，grep 无 erase 路径）；
- local 后端 fallback 模式下 `fallback_paths_` / `fallback_generations_` 只插不删
  （`local.cpp:312、323、540`），readdir enrich 遍历大目录会永久留下等量路径串；
- `poisoned_` 集合只插不删（`local.cpp:170-173`），且无 ctl 命令可清除中毒标记
  ——fsync EIO 后该文件 COMMIT 永久失败，只能重启进程。

### 1.6 P1：伪文件系统 fileid 跨重启不稳定——旧句柄静默指错节点

伪节点 id 是构造时的自增计数器（`core/pseudofs.cpp:9、37`），`oid_of` 直接 memcpy
该 id（:68-73）。重启后（尤其导出增删后）老的伪 filehandle 会解析到**不同节点**——
不是 STALE，是静默指错，比 STALE 危险。修复：伪 oid 用导出路径哈希派生，或混入
boot epoch 令其显式 STALE。同族问题：伪节点 `change` 恒为 1（`pseudofs.cpp:63`），
重启/重配后客户端缓存无法失效，应改用 boot epoch。

### 1.7 P1：协议行为不一致与标识问题

- `op_bind_conn`（`nfsv4/engine.cpp:3024`）对 `CDFC4_BACK` 请求授予 `CDFS4_BACK`，
  与 CREATE_SESSION 声明的"无 backchannel"（`engine.cpp:2967` flags 恒 0）矛盾。
  在实现回传通道之前应无条件只授 FORE。
- `server_owner.major_id` / `server_scope` 硬编码 `"lightnfs"`
  （`nfsv4/engine.cpp:2892-2893`）。两台内容不同的 lightnfs 会被客户端按
  RFC 8881 §2.10.4 误判为同一服务器的 trunking 路径。应配置化，默认从
  hostname + state_dir 派生。
- `state_mgr.cpp:406` 在 shard 锁内做同步 state_dir 文件写入，违反"锁内不做 IO"
  的自述不变式（`state_mgr.hpp:9-13`）；建议移出锁或改为异步落盘。

### 1.8 P1：管理面加固

- ctl unix socket（`server/ctl.cpp:99-110`）不做 `SO_PEERCRED` 校验、不显式
  `fchmod`，权限取决于 umask，而 `expire-client` 是破坏性操作。协议上只做一次
  256 字节 recv（`ctl.cpp:125-126`），命令被拆包即静默截断。
- metrics HTTP 端点绑定 `in6addr_any` 且无 ACL（`ctl.cpp:157-162`），默认对全网
  暴露；无读超时，半开连接占住协程。应加 bind 地址配置 + CIDR ACL + 超时。
- local 后端多处静默吞错：`fchown` 失败空 if 体（`local.cpp:647-648、877-881、
  908-912`）、`(void)::fchmod`（:821）。非 root 部署下新建文件属主错误却无日志无指标。

---

## 2. 性能优化

架构（连接钉在 reactor、协程全异步）是对的，但实现层在热路径上留了大量"每请求
固定开销"。以下按预期收益排序；#1–#4 建议合并为一个"热路径优化"里程碑，用
`bench fullpath` 与真实挂载的 fio 双重验证。

### 2.1 消除每请求的强制 offload + open：resolve 路径加 O_PATH fd 缓存 ⭐

`LocalBackend::resolve`（`local.cpp:406-407`）无条件 offload 一次
`open_oid(O_PATH)`，而引擎在**每个请求/每个 COMPOUND 操作**开头都要 resolve
（`nfsv3/engine.cpp:105-112`、`nfsv4/engine.cpp:137-155`）。现有 fd cache 只覆盖
READ/WRITE/COMMIT 的数据 fd，resolve 完全没有缓存。这是全服务器最大的单点开销：
每请求一次全局 offload 队列往返 + 一次 open 系统调用 + fd 创建销毁 + eventfd 唤醒。

- 后端侧：为 O_PATH fd 建立与数据 fd cache 并列（或合一）的缓存；
- 引擎侧：v4 增加 per-COMPOUND 的 `{cfh → Resolved}` 缓存——`SEQUENCE,PUTFH,GETATTR`
  这类典型链目前对同一 CFH 重复 resolve。

### 2.2 runtime 快路径：去掉每请求 3~5 次多余系统调用 ⭐

`Reactor::post`（`runtime/reactor.cpp:27-30`）无条件 `ring_.wake()`（一次真实
eventfd write），没有"调用方就在本 reactor 线程"的快路径判断——而同线程 post 遍布
热路径：每请求 `spawn` handler（`transport/connection.cpp:102`）、所有同步原语的
`resume_via_post`（`runtime/sync.hpp:46`，AsyncMutex/SharedMutex/CondVar/Semaphore/
Event 全部经由它）、每个 reply 的 `wmu_` 串行锁（`transport/record_stream.cpp:74`）。

- `post()` 判断 `current_reactor_or_null() == this`，是则入本地队列跳过 wake；
- `resume_via_post` 同理直接入本地 ready 队列（避免递归 resume 爆栈）；
- 顺带修 `reactor.cpp:60-61`：`drain` 的 swap 把带容量的 vector 换出去析构，
  MpscQueue 容量每轮清零、每批 push 重新 malloc——把 drain 缓冲提为 Reactor 成员。

### 2.3 io_uring 现代化

setup flags 恒为 0（`uring_ring.cpp:46`），先进特性一个未用，而"一 reactor 一 ring
一线程"是 `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN` 的教科书场景：

| 项 | 改动 | 备注 |
|---|---|---|
| `SINGLE_ISSUER + DEFER_TASKRUN + COOP_TASKRUN` | 一行 + 内核版本探测回退 | 收益直接，先做 |
| `IORING_SETUP_CQSIZE` | CQ 深度与并发度对齐 | 现 SQ=1024/CQ=2048，与 4096 连接 × 64 inflight 脱钩（`runtime.hpp:24`、`connection.hpp:26-27`） |
| multishot accept + 每 reactor `SO_REUSEPORT` listener | 改 `listener.cpp:19-83` | 同时消除 §2.6 的 accept 串行点 |
| registered buffers / provided buffer ring | 依赖 §2.5 buffer pool 改造（需稳定内存区，现在 `std::malloc` 地址不稳定） | 二期 |
| send zerocopy | 大 READ 应答收益 | 二期，需完成通知处理 |
| SQPOLL | 设计 02 §2.63 声明"可配"但配置项与代码均不存在 | 补配置或修文档 |

### 2.4 内存与拷贝路径

- **WRITE 整包拷贝（设计与实现不符）**：设计 02 声明大 opaque 不拷贝，实际
  `xdr.hpp:102-128` 仅当字段落在单 segment 内才零拷贝，而接收缓冲固定 64KB
  （`record_stream.cpp:13`），任何 >64KB 的 WRITE 必然跨段 → 每 1MB 写多一次 1MB
  堆分配 + memcpy。根因是后端 API 只收扁平 span（`backend/api.hpp:204`）。
  修复：后端加 `writev(iovec span)` 重载走 `IORING_OP_WRITEV`，把接收链直接递给内核。
- **BufferPool**（`runtime/buffer.{hpp,cpp}`）：全局 `std::mutex` freelist +
  原子引用计数，而 99% 分配是同 reactor 的——加 thread-local magazine；尺寸类
  4K/64K/1M 跨度过大，128K/256K 的典型 rsize 落到 1M 类浪费 4~8 倍并击穿 64MB
  水位（`buffer.cpp:33-38`）——补中间尺寸类；`alloc` 失败抛 `bad_alloc` 而协程
  未捕获异常直接 `abort()`（`reactor.cpp:114-117`）——OOM 应转为出错降级。
- **协程帧分配**：`TaskPromise` 未重载 `operator new`（`runtime/task.hpp:31-59`），
  一次 GETATTR 的调用链 7~8 个协程帧全走全局堆。加 per-reactor freelist 分配器，
  这是自研 runtime 本应兑现的优势。
- **v4 编码 staging 双重拷贝**：COMPOUND 主循环对每个 op 先编进 staging `XdrEnc`
  再 `to_bytes()` 再拷入主 buffer（`nfsv4/engine.cpp:349-361`），attrs 编码再叠一层
  （`nfsv4/attrs.cpp:54-110`）；READDIRPLUS 每条目一次（`engine.cpp:1084-1110`，
  千条目 ≈ 2000+ 次 vector 分配）。给 `XdrEnc` 加 `mark()/rollback()` 直接编码进
  主 buffer、超预算回滚，两层 staging 都能去掉。
- 小件：reply 的两个临时 `std::vector<iovec>` 改 `SmallVec`
  （`record_stream.cpp:79-86`）；DRC `Claim.cached` 持锁值拷贝改共享引用
  （`rpc/drc.cpp:52`）；DRC Key 去 `std::string peer` 化（`drc.hpp:33`，每请求
  `inet_ntop` + 字符串哈希）；ConnTracker 的 `map<string,int>` + 全局锁改二进制
  key + 分片（`connection.cpp:42-59`）。

### 2.5 offload pool 改造

`runtime/offload_pool.{hpp,cpp}`：单把全局锁 + 无界 FIFO deque，`queue_depth()`
是死代码。local 后端 20+ 处元数据操作全走它，每次 submit/完成回投都抢同一把锁。

- 接入背压（队列深度上限 + 准入等待），`queue_depth` 导出为指标（设计 08 §8.3 本就要求）；
- 分片队列或 per-reactor 队列 + work stealing，消除全局锁；
- 轻重分级（statx/open vs fsync/fallocate），避免慢 fsync 队头阻塞元数据操作。

### 2.6 其余热点（中优先级）

- 四个 listener + lease scanner 全部 spawn 在 reactor 0（`main.cpp:176、237-255`），
  明确热点；配合 §2.3 的 REUSEPORT 方案分散。
- metrics 原子量无 padding 无分片（`obs/metrics.hpp:13-33`），每请求 ≥2 次
  fetch_add 打同一批 cache line——per-reactor 分片累加，导出时汇总。
- `ObjLockRegistry::get` 用 `operator[]` + 命中也插条目 + 超 1024 全表清扫
  （`core/obj_lock.cpp:5-21`）——`find` 快路径 + 摊销清理。
- StateMgr：`clientid → ClientRec` 查找是全 16 分片顺序加锁扫描（插入按 owner 哈希
  分片而 `client_shard()` 是死代码，`state_mgr.cpp:74-76、113-121`），CREATE_SESSION/
  RECLAIM_COMPLETE 等都走它——加 clientid 索引；session 分片单 CondVar 惊群
  （`state_mgr.cpp:557` notify_all 唤醒全分片等待者）——下沉 per-session/slot event；
  `scan_leases` 每秒全表扫描（:1219-1271）——改到期时间堆；分片数 16 硬编码不可调。
- attr 路径：`attr_reply` 里 `mounted_on_fileid` 触发每条目一次 `root()` + `getattr()`
  （`nfsv4/engine.cpp:820-827`，Linux 客户端 READDIR 掩码几乎必含该属性）；
  `limits()/caps()` 与 `supported_attrs` 编码结果均不变，可预计算缓存。
- local 后端：`read()` 每次多一次 statx 求 eof（`local.cpp:591`）；readdir 每页
  重新 open 目录 fd（:497）；DRC 淘汰链 `std::list<Key>` 每项一次节点分配。
- epoll 回退路径：`socks_` 用 `std::map<int,·>`（fd 稠密应数组直索引，
  `epoll_ring.hpp:68`）；ready 非空时跳过 epoll_wait 推迟事件发现（`epoll_ring.cpp:252`）。

---

## 3. 可观测性补齐

现状与设计 08 §8.3/8.4 的差距是系统性的，建议作为一个独立里程碑：

1. **v4 指标为零**：只有 v3 的 per-proc calls/errors/duration 数组
   （`obs/metrics.hpp:16-31`），v4 无任何 per-op 调用数/错误数/时延——想知道 v4
   GETATTR QPS 都做不到。补 per-op 计数 + COMPOUND 时延。
2. **无延迟直方图**：现在只有 duration 累计和，算不出设计 SLI 要求的 p99。
   引入固定 bucket 直方图（Prometheus histogram 语义）。
3. **无 per-export 维度**：全部指标进程全局，多导出无法定位。加 `fsid`/`export` 标签。
4. **文档承诺未兑现**：deployment.md 宣称的 `lightnfs_v4_lock_states /
   lock_segments / lock_denied_total`，`StateMgr::Stats` 字段存在
   （`state_mgr.hpp:287-288`）但 provider（`main.cpp:190-205`）与 `ctl state`
   （`ctl.cpp:70-76`）都没输出。
5. **runtime 层指标全缺**：offload 队列深度、buffer 池水位、reactor 循环延迟、
   fd cache 命中率（现仅 ctl 文本）——全部接入 Prometheus。
6. **慢请求日志与进程内 span**（设计 08 §8.4 要求）：超阈值请求自动落带耗时分解的
   日志，是现网定位的第一工具。
7. errlog 强化：环 64 条硬编码、秒级时间戳、`what[20]` 截断长 op 名
   （`obs/errlog.cpp:16-33`）。
8. `Dispatcher` 的 `std::function` 间接层与 metrics 的 racy `stats()`
   （`drc.cpp:90-93`，TSAN 意义上的 data race）顺带清理。

---

## 4. 运维能力规划

按运维价值排序：

1. **SIGHUP / `ctl reload` 热重载**（最痛）。现在只注册 SIGINT/SIGTERM
   （`main.cpp:283-285`），改任何配置都要重启 → bump boot epoch → 全体客户端
   进 90s grace。分两步走：
   - 第一步（低风险）：热重载不动拓扑的配置——CIDR 白名单、日志级别、限流参数；
   - 第二步：导出动态增删。前置条件是 `PseudoFs` 可重建（现构造后完全不可变，
     `pseudofs.cpp:7-30`）、`ExportTable::entries_` 加并发保护、伪 fs 句柄稳定性
     （§1.6）先修。
2. **ctl 命令面扩展**：`reload`、`loglevel`、`conns`（连接列表）/`kill-conn`、
   `fdcache flush`、`clear-poison`（§1.5 的中毒标记）、`drc flush`、`grace-end`
   （提前结束 grace）、`drain`（优雅摘流）、`version`/`status`、全部命令 `--json`
   输出（脚本现在靠 grep 文本）。协议改为长度前缀 + 循环读（§1.8）。
3. **限速 / QoS**：目前只有并发计数（max_connections / per_peer_limit /
   inflight_per_conn），无任何带宽/IOPS 限制。规划 per-export 与 per-client
   （clientid 级）令牌桶，接在引擎入口。
4. **配置项补齐**：监听 bind 地址（现在只有端口）；`lease` 与 `grace` 解绑
   （`config.cpp:246-247` 强制相等，运维常需 grace < lease 加快恢复）；日志文件/
   轮转/per-module 级别（spdlog 能力已具备未暴露）；状态资源上限（§1.5）；
   状态表分片数可调（§2.6）。
5. **打包**：`packaging/` 现在只有 systemd unit，补 tarball/deb/rpm 构建脚本。

---

## 5. 协议功能规划

按"价值 / 实现成本"排序，并给出建议的接受条件：

### 5.1 短期（v1.x，低成本高价值）

| 特性 | 理由 | 现状锚点 |
|---|---|---|
| **READ_PLUS**（RFC 7862） | 稀疏能力已具备（SEEK/ALLOCATE/DEALLOCATE 都实现了），READ_PLUS 是最低垂的果实；稀疏文件读放大直接受益 | 现返回 NOTSUPP（`nfsv4/engine.cpp:606-615`） |
| **cookieverf 语义化** | 现恒为 0 且不校验（v3 `nfsv3/engine.cpp:363-365`、v4 解出即忽略 `nfsv4/engine.cpp:1016`），分页期间目录被改时客户端静默拿到重复/漏条目。用目录 change attr 生成 verifier | readdir 一致性红线 |
| **xattr 支持**（RFC 8276，op 72-75） | 现在这四个 op 超出 `kLastKnownOp=71` 回 OP_ILLEGAL 而非 NOTSUPP（`nfs4_types.hpp:84-85`），先把表尾提到 75 修正应答语义（半小时），再评估完整实现 | 属性表也缺 `xattr_support` bit 82 |
| v4 READDIR 每页后端批量从硬编码 128 提到按预算计算 | v3 已按 dircount 算到 4096（`nfsv3/engine.cpp:395`），v4 大目录多付数十倍后端往返 | `nfsv4/engine.cpp:1067` |
| LocalObject 实现 `open()` | 设计 05 §5.5 要求 v4 OpenState 持独立 fd，现落到基类 EOPNOTSUPP（`api.hpp` 基类、`nfsv4/engine.cpp:1502-1508` 显式容忍），share deny 在 fs 层无对应语义 | `local.hpp:98-135` |

### 5.2 中期（v2 主题：委托与回传通道）

**读委托 + callback channel 发送侧**是 09 册 M8 的 demand-gated 项，建议作为 v2
的主线：它同时解锁三件事——

- 读委托本身（客户端缓存一致性、GETATTR 风暴削减）；
- `CB_NOTIFY_LOCK`：现在锁冲突只能 DENIED + 客户端盲轮询（`state/lock_mgr.hpp:6-7`），
  高竞争延迟不可控；
- 为将来目录委托/pNFS 铺路。

前置工作已知：StateMgr 增加 delegation 状态类型（现 `StateType` 只有 kOpen/kLock，
`state_mgr.hpp:76`）、CREATE_SESSION 的 backchannel 协商真正启用
（现 "accepted verbatim; never used"，`state_mgr.cpp:351`）、召回超时与
CB_RECALL 失败的降级路径。

同期可做：**异步 COPY**（OFFLOAD_STATUS/OFFLOAD_CANCEL/CB_OFFLOAD）。现在 COPY
无条件同步（`engine.cpp:2745`），且**持源和目标两把独占 ObjLock 同步跑完全量拷贝**
（`engine.cpp:2711-2725`，`ca_count==0` 时长度无上限）——大文件 COPY 会把该文件上
所有请求阻塞任意久。即使不做完整异步，也应先做分片拷贝 + 中途放锁。

### 5.3 中期（另一条线：第二后端）

09 册定义的"接口冻结真实检验"。启动前先落两笔利息：

- 能力位体系有 5 个位处于半接线状态（audit 见后端审查）：`kNativeAccess` 无人
  置位无人消费、`kNativeChange` 置了没人消费（多网关一致性的前提）、`kByteLocks` /
  `backend::LockMgr` 抽象类是纯死代码（`api.hpp:237-244`）、`kJukebox` 端到端不可达
  （Lustre HSM 正需要它）、`kCaseInsensitive` 只有消费端。第二后端会真实用到
  NativeChange/Jukebox，先把链路打通。
- `OpenCtx`（`api.hpp:150-153`）现在恒为空壳（见 §5.1 的 `open()`）。

### 5.4 长期观察（维持 09 册口径，补充新证据）

- RPC-over-TLS（RFC 9289）与 RPCSEC_GSS：AUTH_SYS-only 仍是最大的部署边界；
- NLM/NSM（v3 锁）：按需；
- pNFS flexfiles、写/目录委托：前提未变；
- 多网关一致性：依赖 kNativeChange + kByteLocks 后端（见 §5.3）。

---

## 6. 结构性重构

均为降低后续演进成本的投资，可穿插在功能里程碑之间：

1. **mutate-guard 下沉 core**：resolve → readonly → 名字校验 → squash → 独占锁 →
   WCC before/after 采样这套序列，v3 重复 8 处（`nfsv3/engine.cpp:551-1035` 各写
   过程）、v4 重复 6 处，且两边 readonly 与名字校验的求值顺序不一致。抽成
   `core::MutateGuard` 协程 helper，顺带统一顺序。
2. **名字合法性校验三套合一**：v3（`nfsv3/engine.cpp:63-71`）、v4
   （`nfsv4/engine.cpp:41-66`）、mountd（`mountd/mount3.cpp:43-46`）各写一遍
   NUL/斜杠/dot/长度判断；UTF-8 部分保留 v4 特有。
3. **WCC / change_info 采样统一**：`sample()`（v3）与 `change_of()`（v4）是同一
   红线语义的两份实现，core 提供统一 `ChangeSample`，引擎只管编码。
4. **属性映射去重**：caps/limits → FSINFO/PATHCONF 与 v4 属性位的推导、
   statfs → 空间属性的展开，v3/v4 各有一份。
5. **死代码清理**：`Reactor::op_finished`、`StateMgr::client_shard`、
   `OffloadPool::queue_depth`（接入指标后即非死代码）、`state_mgr.cpp:922-924`
   过时注释。
6. v3 `dispatch_proc` 的 22 连 `if` 改跳转表/switch（`nfsv3/engine.cpp:157-180`）。

---

## 7. 测试与工程化

### 7.1 测试缺口

- **零单测模块**：`server/ctl.cpp`（含 metrics HTTP）、`obs/metrics.cpp`、
  `server/rpcbind.cpp`。ctl 仅被验收脚本冒烟。
- backend：fd cache 容量/淘汰/并发压力（§1.3 修复的回归）、kernel-handle 模式
  （现有测试只断言 fallback 路径）、identity strict/setfsuid（§1.1 修复后补端到端）、
  poison 语义单测。
- v4.2 边界：CLONE 非块对齐（FICLONERANGE EINVAL 透传无用例，
  `local.cpp:1150-1154`）、同文件重叠 COPY、超 EOF 的 ALLOCATE/DEALLOCATE、
  `max_filesize` 边界。
- 并发/长稳：§1.2 数据竞争的针对性 TSAN 用例；fd 泄漏与内存增长的长稳回归
  （§1.5 的三处无界增长正需要它）。
- 故障注入面太窄：目前只有 `LNFS_FAULT_FSYNC_EIO` 一个钩子（`local.cpp:998-1012`），
  补 ENOSPC/EDQUOT/读 EIO/短写/慢盘、网络 RST/半关闭。

### 7.2 fuzz 扩面

现在只有 1 个 target（`fuzz/fuzz_handle_request.cpp`）、17 个语料。新增 target：
`core/file_handle` 解码（HMAC 边界，`lightnfs-fh` 直接吃用户 hex）、
`LocalBackend::open_oid` 的 ObjId 解析（`local.cpp:328-361`，句柄内容客户端可影响）、
自研 TOML 解析器（`core/config.cpp`）、record_stream 分片重组（现在喂的是完整记录）、
v4 attrs bitmap。配 fuzz 字典、语料最小化脚本，target 去掉真实文件系统依赖
（现 state_dir 落 /tmp，非 hermetic）。

### 7.3 工程化

- **README 与实际不符处修正或补齐**：宣称的 GCC/Clang × TSAN × epoll 构建矩阵
  没有脚本承载（build-tsan/、build-epoll/ 是手工产物）；`gen_errmap_cases.py --check`
  与 `bench_gate.sh` 未接入 ctest；宣称的每 PR 120s / 夜间 1h fuzz 无驱动脚本。
  建议补 `scripts/ci.sh` 一站承载（本地或任意调度器可跑），并把 errmap check
  注册进 ctest。
- 外部依赖固定 commit：`fetch_{cthon,fsx,pynfs}.sh` 都是 clone HEAD，不可复现。
- 孤儿清理：`gen_dataset.sh` 无引用；`docs/test-report.md` 引用的
  `pynfs_m4_expected.txt` 不存在。
- lint：有 `.clang-format` 无检查脚本；评估 clang-tidy 与覆盖率（llvm-cov）接入。

---

## 8. 分阶段执行建议

| 阶段 | 主题 | 内容 | 验收 |
|---|---|---|---|
| v1.1 | 缺陷修复 | §1 全部（P0 一周内、P1 一月内），每项带回归测试 | TSAN/ASAN 全绿；新增回归全过 |
| v1.2 | 热路径性能 | §2.1–§2.4（resolve 缓存、post 快路径、uring flags、writev/编码拷贝、buffer pool） | `bench fullpath` 与 fio（vers=3/4.2）对 baseline 的提升入库为新 baseline |
| v1.3 | 可观测性 + 运维 | §3 全部；§4.1 第一步热重载 + §4.2 ctl 扩展 | p99 可从 Prometheus 算出；reload 不中断 IO 的验收用例 |
| v1.4 | 协议短板 | §5.1（READ_PLUS、cookieverf、xattr 应答语义、open()） | pynfs 对应组通过；稀疏读基准 |
| v2.0 | 委托 + 回传通道 | §5.2；穿插 §6 重构 | CB_RECALL/CB_NOTIFY_LOCK 真实客户端验收；委托削减 GETATTR 的量化 |
| v2.x | 第二后端 | §5.3，先补能力位链路 | 09 册"接口冻结检验"的 DoD |

测试与工程化（§7）不单列阶段，按"谁改到谁补齐"原则摊入各阶段，v1.1 优先补
fuzz 扩面与故障注入钩子。
