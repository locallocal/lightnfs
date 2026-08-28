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

### 1.2 P0：lock stateid 检查中的数据竞争 ✅ 已修复

`state/state_mgr.cpp:824` 在 state 分片锁已释放（:810 作用域结束）后读取
`open.bopen`（父 open state 的 `shared_ptr<backend::Object>`），而并发 CLOSE 走
`unlink_state()`（`state_mgr.cpp:586`）在**另一个分片**的锁下对同一成员做
`std::move`。"持锁拷贝"的自述不变式（`state_mgr.hpp:96-99`）被违反，
拷贝 vs 移动构成 UB。用 lock stateid 做 READ/WRITE 与并发 CLOSE 父 open 即可触发。

- 修复：按父 open 的 `other` 重新加对应分片锁后再取 `bopen`（保持"一次一锁"纪律）。
- 回归：加针对性并发测试并纳入 TSAN 构建。

### 1.3 P0：fd cache 无硬上限，可耗尽进程 fd ✅ 已修复

`backend/local.cpp:127-137` 的淘汰循环在"全片条目都在使用中"时 `break`，缓存越过
容量继续增长；高并发下 fd 数可突破 RLIMIT_NOFILE，且没有"超容量"计数暴露。
另外淘汰本身是持分片锁的 O(N) 全表扫描（默认每片 256 条），插入路径 O(N²)。

- 修复：改为侵入式 LRU 链表（同时消掉线性扫描），全在用时至少计数 + 告警；
  参见 §2.4 的整体 fd cache 改造。

### 1.4 P0：io_uring 提交路径的两个健壮性缺陷 ✅ 已修复

- `io_uring_submit()` 返回值三处全部被丢弃（`runtime/uring_ring.cpp:57、69、141`）。
  CQ overflow 时 submit 返回 `-EBUSY`，SQE 静默不提交，等待的协程**永久挂起**且无日志。
- `get_sqe()`（`uring_ring.cpp:66-74`）在 SQ 耗尽时靠 `assert` 兜底，`NDEBUG` 下
  返回 nullptr 直接被 `io_uring_prep_*` 解引用。

### 1.5 P1：状态资源无上限（DoS 面） ✅ 已修复

`NFS4ERR_RESOURCE` 已定义（`nfsv4/nfs4_types.hpp:131`）但从未被返回；没有
max clients / max opens per client / max lock segments 任何限制。失控客户端可无限
创建 open/lock state 撑爆内存。配套的还有两处无界增长：

- `lock_owners_` 表只插不删（`state_mgr.hpp:352-353`，grep 无 erase 路径）；
- local 后端 fallback 模式下 `fallback_paths_` / `fallback_generations_` 只插不删
  （`local.cpp:312、323、540`），readdir enrich 遍历大目录会永久留下等量路径串；
- `poisoned_` 集合只插不删（`local.cpp:170-173`），且无 ctl 命令可清除中毒标记
  ——fsync EIO 后该文件 COMMIT 永久失败，只能重启进程。

### 1.6 P1：伪文件系统 fileid 跨重启不稳定——旧句柄静默指错节点 ✅ 已修复

伪节点 id 是构造时的自增计数器（`core/pseudofs.cpp:9、37`），`oid_of` 直接 memcpy
该 id（:68-73）。重启后（尤其导出增删后）老的伪 filehandle 会解析到**不同节点**——
不是 STALE，是静默指错，比 STALE 危险。修复：伪 oid 用导出路径哈希派生，或混入
boot epoch 令其显式 STALE。同族问题：伪节点 `change` 恒为 1（`pseudofs.cpp:63`），
重启/重配后客户端缓存无法失效，应改用 boot epoch。

### 1.7 P1：协议行为不一致与标识问题 ✅ 已修复

- `op_bind_conn`（`nfsv4/engine.cpp:3024`）对 `CDFC4_BACK` 请求授予 `CDFS4_BACK`，
  与 CREATE_SESSION 声明的"无 backchannel"（`engine.cpp:2967` flags 恒 0）矛盾。
  在实现回传通道之前应无条件只授 FORE。
- `server_owner.major_id` / `server_scope` 硬编码 `"lightnfs"`
  （`nfsv4/engine.cpp:2892-2893`）。两台内容不同的 lightnfs 会被客户端按
  RFC 8881 §2.10.4 误判为同一服务器的 trunking 路径。应配置化，默认从
  hostname + state_dir 派生。
- `state_mgr.cpp:406` 在 shard 锁内做同步 state_dir 文件写入，违反"锁内不做 IO"
  的自述不变式（`state_mgr.hpp:9-13`）；建议移出锁或改为异步落盘。

### 1.8 P1：管理面加固 ✅ 已修复

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

### 2.1 消除每请求的强制 offload + open：resolve 路径加 O_PATH fd 缓存 ⭐ ✅ 已完成

`LocalBackend::resolve`（`local.cpp:406-407`）无条件 offload 一次
`open_oid(O_PATH)`，而引擎在**每个请求/每个 COMPOUND 操作**开头都要 resolve
（`nfsv3/engine.cpp:105-112`、`nfsv4/engine.cpp:137-155`）。现有 fd cache 只覆盖
READ/WRITE/COMMIT 的数据 fd，resolve 完全没有缓存。这是全服务器最大的单点开销：
每请求一次全局 offload 队列往返 + 一次 open 系统调用 + fd 创建销毁 + eventfd 唤醒。

- 后端侧：为 O_PATH fd 建立与数据 fd cache 并列（或合一）的缓存；
- 引擎侧：v4 增加 per-COMPOUND 的 `{cfh → Resolved}` 缓存——`SEQUENCE,PUTFH,GETATTR`
  这类典型链目前对同一 CFH 重复 resolve。

### 2.2 runtime 快路径：去掉每请求 3~5 次多余系统调用 ⭐ ✅ 已完成

`Reactor::post`（`runtime/reactor.cpp:27-30`）无条件 `ring_.wake()`（一次真实
eventfd write），没有"调用方就在本 reactor 线程"的快路径判断——而同线程 post 遍布
热路径：每请求 `spawn` handler（`transport/connection.cpp:102`）、所有同步原语的
`resume_via_post`（`runtime/sync.hpp:46`，AsyncMutex/SharedMutex/CondVar/Semaphore/
Event 全部经由它）、每个 reply 的 `wmu_` 串行锁（`transport/record_stream.cpp:74`）。

- `post()` 判断 `current_reactor_or_null() == this`，是则入本地队列跳过 wake；
- `resume_via_post` 同理直接入本地 ready 队列（避免递归 resume 爆栈）；
- 顺带修 `reactor.cpp:60-61`：`drain` 的 swap 把带容量的 vector 换出去析构，
  MpscQueue 容量每轮清零、每批 push 重新 malloc——把 drain 缓冲提为 Reactor 成员。

### 2.3 io_uring 现代化 ✅ 已完成（registered buffers / send zerocopy 留二期）

setup flags 恒为 0（`uring_ring.cpp:46`），先进特性一个未用，而"一 reactor 一 ring
一线程"是 `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN` 的教科书场景：

| 项 | 改动 | 备注 |
|---|---|---|
| `SINGLE_ISSUER + DEFER_TASKRUN + COOP_TASKRUN` | ✅ setup 阶梯探测（DEFER→COOP→plain），SINGLE_ISSUER 经 `R_DISABLED` 创建、`bind_submitter()` 在 reactor 线程启用 | `uring_ring.cpp` create()/bind_submitter() |
| `IORING_SETUP_CQSIZE` | ✅ CQ 默认 8×SQ，`Runtime::Config::ring_cq_entries` 可调 | |
| multishot accept + 每 reactor `SO_REUSEPORT` listener | ✅ addr-less accept 走 multishot（每 listener 一个常驻 SQE，5.19+ 探测回退单发）；Listener 改为每 reactor 一个 REUSEPORT socket + 本 reactor accept 循环，连接就地服务 | 同时消除 §2.6 的 accept 串行点与跨 reactor 交接 |
| registered buffers / provided buffer ring | 依赖稳定内存区（magazine 化后仍是 malloc 地址） | 二期 |
| send zerocopy | 大 READ 应答收益 | 二期，需完成通知处理 |
| SQPOLL | ✅ 配置项 `[server] ring_sqpoll`（含 `ring = auto\|uring\|epoll`），不可用时告警回退 | |

### 2.4 内存与拷贝路径 ✅ 已完成

- **WRITE 整包拷贝（设计与实现不符）** ✅：`XdrDec::opaque_spans()` 把跨段 opaque
  以零拷贝段视图交出（不再 gather）；后端加 `write(iovec span)` 重载，local 走
  `IORING_OP_WRITEV`（短写续传），默认实现逐段透传；v3/v4 WRITE 均改走段路径。
- **BufferPool** ✅：thread-local magazine（每线程每尺寸类 16 块，池亡/线程退出经
  全局注册表安全回收）；尺寸类补 128K/256K（现 4K/64K/128K/256K/1M）；OOM 先清空
  freelist 重试，仍失败在连接层降级为断连而非进程 abort（`handle_one` 兜底 +
  `RecordStream::fill` 返回 ENOMEM）。
- **协程帧分配** ✅：`runtime/frame_alloc.hpp` per-thread 64B 粒度 freelist
  （≤4KB 帧，每 bin 上限 64），`TaskPromise`/spawn root promise 接入
  `operator new/delete`；跨线程释放只是块在线程缓存间迁移，天然安全。
- **v4 编码 staging 双重拷贝** ✅：`XdrEnc::mark()/rollback()`（可跨 tail 关闭与
  attach 回滚，早于 mark 的 raw_gap 指针保持有效）；COMPOUND 逐 op、READDIR 逐条目
  改为就地编码 + 超预算回滚；`encode_fattr` 改 attrmask 后留 4 字节 gap、值就地
  编码、末尾 patch 长度，去掉 vals staging。
- 小件 ✅：reply iovec 改 `SmallVec<iovec,8>`；DRC `Claim.cached` 改
  `shared_ptr<const vector<byte>>`；DRC Key 改二进制（16B v6-mapped 地址 + 端口，
  `Key::make(sockaddr_storage)`）；ConnTracker 改 16B 二进制 key + 8 分片 +
  原子总数。

### 2.5 offload pool 改造 ✅ 已完成

`runtime/offload_pool.{hpp,cpp}`：单把全局锁 + 无界 FIFO deque，`queue_depth()`
是死代码。local 后端 20+ 处元数据操作全走它，每次 submit/完成回投都抢同一把锁。

- ✅ 背压：每类 `queue_cap`（默认 4096，`[server] offload_queue_cap`）之上进入
  overflow 准入队列、随完成逐个放行；`deferred` 计数 + 队列深度经
  `lightnfs_offload_*` 指标导出（`queue_depth()` 复活为指标源）。
- ✅ 分片队列 + work stealing：每 worker 一个 shard（mutex+deque），类级
  counting_semaphore 停车；空闲 worker 从自己的 shard 起扫全组（即 stealing），
  全局锁消除。
- ✅ 轻重分级：`OffloadClass::{kLight,kHeavy}`，heavy 独占预留线程
  （默认 max(1, threads/4)，`[server] offload_heavy_threads` 可调；单线程池回落
  共用），local 后端 allocate/deallocate/clone/copy_range 标记 kHeavy——慢 fsync/
  fallocate 不再队头阻塞元数据操作。

### 2.6 其余热点（中优先级） ✅ 已完成

- ✅ listener 已随 §2.3 分散到每 reactor；lease scanner 移到最后一个 reactor，
  ctl / metrics HTTP 分别移到 reactor 1/2（模 reactor 数）——reactor 0 不再是
  辅助任务的聚集点。
- ✅ metrics 改 `ShardedCounter`（8 个 per-thread slot、每 slot 独占 cache line，
  fetch_add/fetch_sub/load 接口不变，导出时求和）。
- ✅ `ObjLockRegistry::get`：`find` 命中快路径（不再每次命中插条目）；超水位后
  每次插入只扫少量哈希桶（轮转游标摊销清理），全表清扫消除。
- ✅ StateMgr：新增 `clientid → ClientRec` 专用索引（`client_idx_`，与 by_id 同步
  维护），CREATE_SESSION/DESTROY_CLIENTID/RECLAIM_COMPLETE/expire 全部 O(1) 定位，
  全分片扫描消除；SEQUENCE 重传等待下沉为 per-slot CondVar（会话销毁前逐 slot 唤
  醒），分片级惊群消除；`scan_leases` 改为到期时间堆（懒惰重臂:续租仍是无锁原子
  store,过期堆项弹出时按实际到期重新入堆,courtesy 复活时重臂新租约到期），
  每秒全表扫描消除；分片数经 `Config::shards`（`[server] state_shards`）可调。
- ✅ attr 路径：`mounted_on_fileid` 的导出根比较改用 per-fsid root oid 缓存
  （`Engine::root_oid_of`），不再每次 `backend->root()`；`supported_attrs` 本就是
  静态单例，limits/caps 为单次虚调用返回值，无需额外缓存层。
- ✅ local 后端：`read()` 的 eof 由短读推断（读满时 eof=false,客户端在精确边界多
  一次空读换掉每次 READ 的 statx；零长读保留 size 探测）；readdir 页复用 fd cache
  的目录 fd（per-entry dents 互斥串行化共享的目录偏移），每页 open/close 消除；
  DRC 淘汰链 `std::list` → `std::deque`。
- ✅ epoll 回退路径：`socks_` 改 fd 直索引的稠密表
  （`vector<unique_ptr<FdQ>>`）；ready 非空时以 0 超时轮询 epoll 而非跳过，
  事件发现不再被推迟。

---

## 3. 可观测性补齐 ✅ 已完成（2026-08-28）

现状与设计 08 §8.3/8.4 的差距是系统性的，建议作为一个独立里程碑：

1. **v4 指标为零** ✅：`exec_op` 包装层按 opcode 记 per-op calls/errors/时延直方图
   （`lightnfs_v4_op_*{op=...}`，op 名在 bump 时存入 obs，obs 不依赖 nfsv4 表），
   另有整 COMPOUND 直方图 `lightnfs_v4_compound_duration_seconds`。
2. **无延迟直方图** ✅：`obs::LatencyHistogram`（固定桶 100µs–5s，Prometheus
   histogram 语义，per-thread slot 无争用）；v3 per-proc、v4 per-op/COMPOUND、
   reactor 循环均接入，p99 可从 exposition 算出。
3. **无 per-export 维度** ✅：`ExportEntry` 持 `obs::ExportMetrics`，READ/WRITE/COPY
   路径 bump；导出为带 `{export,fsid}` 标签的
   `lightnfs_export_{read,write}_{bytes,ops}_total`。
4. **文档承诺未兑现** ✅：provider 输出 `lightnfs_v4_lock_states / lock_segments /
   lock_owners / lock_denied_total`；`ctl state` 首行同步补齐。
5. **runtime 层指标全缺** ✅：offload 组（§2.5 已有）之外补 reactor 循环忙时直方图
   （`Reactor::loop_stats()`，reactor 线程 relaxed 写）、
   `lightnfs_buffer_pool_free_bytes{listener}`、带标签的 `lightnfs_fdcache_*`。
6. **慢请求日志与进程内 span** ✅：`[server] slow_request_ms`（默认 1000，0 关）；
   v4 在 Ctx 内零分配记录前 32 个 op 的耗时，超阈值 warn 日志附
   `ops=[PUTFH=12us,...]` 分解；v3 落单过程慢日志。
7. errlog 强化 ✅：环大小 `[server] error_ring` 可配（默认 64）、毫秒时间戳、
   `what[32]` 容纳最长 v4 op 名（BIND_CONN_TO_SESSION）。
8. `Dispatcher`/`Drc` 清理 ✅：Handler 改函数指针 + self（去 `std::function`
   型别擦除间接层）；DRC 分片 `bytes`/条目数改原子，`stats()` 不再有 TSAN 意义
   上的 data race。

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
