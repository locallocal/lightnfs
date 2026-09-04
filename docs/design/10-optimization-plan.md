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

## 4. 运维能力规划 ✅ 已完成（2026-08-28；4.1 第二步除外，见说明）

按运维价值排序：

1. **SIGHUP / `ctl reload` 热重载**（最痛）。分两步走：
   - 第一步 ✅：SIGHUP（主线程 wait 循环应用）与 `ctl reload` 共用一个 handler，
     重解析并校验配置后应用非拓扑子集——日志级别、slow_request_ms、error_ring、
     每导出 clients CIDR（`ExportEntry::set_clients`，原子指针发布 + 退休链，读侧
     零锁）与 QoS 速率、per-client QoS；拓扑/线程/监听/state_dir 改动在报告中标注
     restart required。systemd `ExecReload` 已接 SIGHUP。
   - 第二步（导出动态增删）**仍未做**：前置条件不变——`PseudoFs` 可重建、
     `ExportTable::entries_` 并发保护（伪 fs 句柄稳定性 §1.6 已修）。列入 v2 前置。
2. **ctl 命令面扩展** ✅：`reload`、`loglevel`、`conns`/`kill-conn`（进程级
   `ConnRegistry`，注册表存在性证明 fd 未复用、kill 走 shutdown）、`fdcache flush`
   （淘汰全部未 pin 条目）、`drc flush`、`grace-end`（`StateMgr::end_grace`）、
   `drain`（停 accept 循环、存量继续服务）、`version`/`status`，全部命令支持
   `--json`。协议维持"换行终止 + 循环读"（§1.8 已修的分包问题即其动机；单命令单
   连接下与长度前缀等价，不再改）。
3. **限速 / QoS** ✅：`rt::TokenBucket`（异步、debt 模式放行超突发请求、未配置时
   一次 relaxed load 即返回、refill 时钟取自 reactor 便于 fake-clock 测试）；
   per-export（`[[export]] read_bps/write_bps/iops`，挂 `ExportEntry::qos`）与
   per-client（v4 clientid，`[limits] client_*`，engine 内懒建桶表）都接在引擎
   READ/WRITE 入口、对象锁之前；速率热重载。
4. **配置项补齐** ✅：`[server] bind` 监听地址（listener 支持 v4/v6 字面量，空 =
   双栈全接口）；`grace` 与 `lease` 解绑（`auto` = lease）；`log_file` +
   `log_rotate_size/keep`（spdlog rotating sink）；状态资源上限（§1.5 已有）与
   分片数（§2.6 已有）。per-module 日志级别**未暴露**：门面是单 logger，按模块拆
   logger 需逐调用点带模块标签，收益不抵改动面，搁置。
5. **打包** ✅：`packaging/make_tarball.sh`（/usr/local 布局 tar.gz）、
   `make_deb.sh`（dpkg-deb）、`make_rpm.sh` + `lightnfs.spec`（rpmbuild），三者
   共享 CMake install 文件集（`GNUInstallDirs` + `cmake --install` DESTDIR 分阶段）；
   版本号来自 `project(lightnfs VERSION …)`，二进制与 ctl `version` 同源
   （`LIGHTNFS_VERSION`）。

---

## 5. 协议功能规划

按"价值 / 实现成本"排序，并给出建议的接受条件：

### 5.1 短期（v1.x，低成本高价值） ✅ 已完成（2026-08-28）

| 特性 | 落地方式 |
|---|---|
| **READ_PLUS**（RFC 7862） ✅ | `op_read_plus`：每应答一个段——offset 落在洞里返回 HOLE 段（clamp 到请求与 EOF），落在数据上先 SEEK_HOLE 截到洞边界再按 READ 路径返回 DATA 段；客户端从段尾自续传（RFC 允许的短应答范式）。无 kSparseOps 的后端退化为单 DATA 段；stateid/budget/QoS 检查与 READ 完全一致。minor 1 仍 OP_ILLEGAL（表到 58）。内部洞的段编码由 SEEK 语义单测 + 后续 pynfs 4.2 验收覆盖（memory 后端无内部洞，引擎单测覆盖 DATA/EOF/空段路径） |
| **cookieverf 语义化** ✅ | v3/v4 verifier = 目录 change 属性（8 字节）；cookie≠0 时校验，不匹配回 BAD_COOKIE（v3）/NOT_SAME（v4），客户端从 cookie 0 重启获得一致列举。cookie==0 忽略 verifier（RFC 语义）。顺带修正 memory 测试后端播种路径不打目录 change 的保真缺口 |
| **xattr 应答语义**（RFC 8276） ✅ | 表尾 `kLastKnownOp` 提到 75，op 72-75（GETXATTR/SETXATTR/LISTXATTRS/REMOVEXATTR）在 minor 2 回 NOTSUPP、minor 1 仍 ILLEGAL；op_name/指标数组同步。`xattr_support` bit 82 未加：不宣告该属性与宣告 false 对客户端等效（Linux 客户端按属性缺失判定无 xattr），留给完整实现一并做 |
| v4 READDIR 批量按预算 ✅ | 每页后端批量 = clamp(dircount/24, 16, 4096)，对齐 v3 |
| LocalObject `open()` ✅ | `LocalOpenState` 持每 OPEN 独立 fd（写开 O_RDWR）；read/write/seek 优先用它——open 时定权限的 POSIX 语义（OPEN 后 chmod 不再断读写）、免 fd 缓存往返；打开失败降级 EOPNOTSUPP 保持旧行为；合并升级后不可写句柄自动回落 fd 缓存路径。share deny 仍由状态层唯一执行（fs 层本无对应语义，维持设计 07 口径） |

### 5.2 中期（v2 主题：委托与回传通道） ✅ 已完成（2026-08-28；异步 COPY 取分片方案，见末段）

**读委托 + callback channel 发送侧**已落地：

- **回传通道发送侧**：`transport::CbChannel`（每连接一个发送句柄，状态层持
  shared_ptr 跨连接生命周期；发送任务投递回连接自己的 reactor 并计入 drain 计数，
  teardown 先 detach——排队未跑的发送观测到 detach 跳过、在途的被 drain 等待，
  悬空 fd/UAF 不可能）；连接读循环按 msg_type 把 RPC REPLY 记录路由给挂起的回调
  （`route_cb_reply`）。CREATE_SESSION 的 CONN_BACK_CHAN 与
  BIND_CONN_TO_SESSION(BACK/BOTH) 真正绑定通道并如实应答（back channel 属性
  clamp 到 1 slot / 2 ops）；回调凭据取客户端 sec_parms 的首个 AUTH_SYS（否则
  AUTH_NONE）。CB_COMPOUND（CB_SEQUENCE 前缀）由 `nfsv4/callback.cpp` 手工编码，
  单 slot 串行；slot 卡死超过一个租约即判通道 down（不依赖定时器）。
  SEQUENCE 的 sr_status_flags 现会报 CB_PATH_DOWN。
- **读委托**：`StateType::kDeleg`；授予策略（开关 `[protocol] delegations`、非
  grace/非 reclaim、只读 OPEN、会话有活回传通道、无他端写打开、每客户端每文件一
  张）在授予与发布同一把文件分片锁下完成，与并发写打开互斥。冲突（写 OPEN 含持有
  者自身、SETATTR、REMOVE/RENAME 被替换目标、匿名写）→ 标记 recalled、发
  CB_RECALL、回 DELAY，客户端 CLAIM_DELEG_CUR_FH 转正打开 + DELEGRETURN 后重试；
  租约期内不归还由租约扫描器吊销。DELEGRETURN/CLAIM_DELEG_CUR_FH 已实现；
  客户端过期/换代走既有 unlink 链自动回收。指标：delegs/grants/recalls/returns/
  revokes 进 Prometheus 与 `ctl state`。
- **CB_NOTIFY_LOCK**：LOCK DENIED 时登记等待者（每文件上限 16、租约期失效、按
  owner 去重）；LOCKU 或锁状态随 CLOSE/过期释放时通知等待客户端重试，替代盲轮询。
- **边界（文档明示）**：v3 侧写不触发召回（与 v3/v4 锁边界同口径）；无写委托/
  目录委托；CLAIM_DELEGATE_PREV（重启后委托 reclaim）不支持——委托随重启消亡，
  普通打开经 CLAIM_PREVIOUS 恢复。

**异步 COPY**：取计划许可的分片方案——COPY 改为 8MiB 分片、片间释放两把 ObjLock
（`ca_count==0` 的全文件拷贝不再把源/目标上的所有请求阻塞任意久）；完整异步
（OFFLOAD_STATUS/OFFLOAD_CANCEL/CB_OFFLOAD）留待有真实需求时基于现有回传通道
实现（发送侧已具备）。

### 5.3 中期（另一条线：第二后端） ✅ 已完成（2026-09-03；GlusterFS/libgfapi）

09 册定义的"接口冻结真实检验"。启动前先落两笔利息：

- 能力位体系有 5 个位处于半接线状态（audit 见后端审查）：`kNativeAccess` 无人
  置位无人消费、`kNativeChange` 置了没人消费（多网关一致性的前提）、`kByteLocks` /
  `backend::LockMgr` 抽象类是纯死代码（`api.hpp:237-244`）、`kJukebox` 端到端不可达
  （Lustre HSM 正需要它）、`kCaseInsensitive` 只有消费端。第二后端会真实用到
  NativeChange/Jukebox，先把链路打通。
- `OpenCtx`（`api.hpp:150-153`）现在恒为空壳（见 §5.1 的 `open()`）。

**利息已付（能力位链路）**：

| 位 | 生产端 | 消费端 |
|---|---|---|
| `kNativeAccess` | gluster（`glfs_h_access` 在调用者 fsuid/fsgid/groups 下由砖块 posix-acl 判定） | v4 OPEN 跳过网关侧 `access()` 预检，直接由后端 `open()` 权威判定（省一次集群往返；契约：置位后端的 `open()` 不得以 EOPNOTSUPP 降级）；`core::FsProps::native_access` |
| `kNativeChange` | local（STATX_CHANGE_COOKIE，原有）；cephfs（`stx_version`，MDS change attribute，2026-09-04） | v4.2 属性 `change_attr_type`(79)：置位 → MONOTONIC_INCR，否则 TIME_METADATA（ctime 合成），伪根 → MONOTONIC_INCR（boot epoch）；`FsProps::native_change`；启动日志 `backend traits` |
| `kByteLocks` / `LockMgr` | gluster `GlusterLockMgr`（`glfs_posix_lock` + `glfs_fd_set_lkowner`，每 (文件, lock-owner) 一个 glfd）；lustre `LustreLockMgr`（OFD）；cephfs `CephLockMgr`（`ceph_ll_setlk`，每 (文件, owner) 一个 Fh） | 状态层 `StateMgr::Config::native_locks` 钩子（main 用 ExportTable 接线）：LOCK 先网关表授予再下推，后端拒绝（EAGAIN）→ 回滚网关授予到此前覆盖、回 DENIED（冲突段取自后端 `test()`，持有者未知 → clientid 0）；后端错误 → DELAY/SERVERFAULT 且不授予；LOCKU 镜像解锁；CLOSE/FREE_STATEID/过期经 `unlink_state` 调新增的 `LockMgr::release()`；LOCKT 本地无冲突时探测后端（同 owner 已覆盖区间时不探测——探测无法区分自身锁）。指标 `lightnfs_v4_native_lock_{denied,errors}_total` |
| `kJukebox` | gluster/cephfs（ENOTCONN/ETIMEDOUT/ENET*/EHOST* 传输类错误 → `kJukebox`，`jukebox=false` 时回 EIO；cephfs 的 EBLOCKLISTED 例外为硬 EIO）；lustre（HSM 已释放文件）；memory/local 经故障注入 `LNFS_FAULT_JUKEBOX=N`（`fault::Kind::kJukebox`） | 既有 errmap：v3 READ/WRITE → JUKEBOX（08 册 §8.2 白名单仅此两处，且二者不进 DRC，故无"缓存了 JUKEBOX"问题），v4 任意 op → DELAY；引擎级测试 `Nfs3.JukeboxReachesTheWireAndRetrySucceeds` / `Nfs4.JukeboxIsDelayAndRetrySucceeds` |
| `kCaseInsensitive` | 仍无生产者（local/gluster/lustre/cephfs 均大小写敏感；留给未来后端） | 原有 |
| `OpenCtx` / `OpenState` | 第二个真实生产者 `GlusterOpenState`（每 OPEN 一个 glfd，以调用者身份打开） | 原有 IO 站点；`static_cast` 不跨后端的前提改为"OpenState 只回到产生它的后端" |

**GlusterFS 后端**（`backend/gluster.{hpp,cpp}`，`backend/gfapi.{hpp,cpp}`）：06 册
§6.6 映射表逐项落地，见该节。要点：

- **运行时加载 libgfapi**（`dlopen("libgfapi.so.0")` + `dlvsym` 版本符号，47 个入口填一张
  函数表），无构建期依赖、构建矩阵不变；`scripts/check_gfapi_abi.sh` 在有
  `glusterfs/api` 头文件的主机上把函数表与真实声明逐一 static_assert（已对 GlusterFS 11
  头文件验证通过；CI 步骤在无头文件时跳过）。
- ObjId = 标记字节 + 16 字节 GFID（`kStableHandles`）；`resolve` = `glfs_h_create_from_handle`
  经分片 LRU 对象句柄缓存；匿名 IO 走每对象 glfd 缓存（读→写升级，以网关身份打开，
  门禁 = 原生 `access()` + v3 属主放宽）；readdir = `glfs_h_opendir` + `glfs_xreaddirplus_r`
  (STAT|HANDLE)，d_off 作 cookie；v4.2：`glfs_lseek(SEEK_DATA/HOLE)`/`glfs_fallocate`/
  `glfs_discard` → kSparseOps，`glfs_copy_file_range`（pread/pwrite 回落）→ kCopyRange，
  无 CLONE；EXCLUSIVE 创建 verifier 存 atime/mtime（与 local 同布局）；fsync 失败粘性
  poison 同 06 §6.2；`start()` 才连接集群（配置加载不阻塞），`stop()` 释放全部句柄后
  `glfs_fini`。配置 `[export.gluster] volume/servers/subdir/transport/log_file/log_level/
  fd_cache/readdir_enrich/jukebox/native_locks`；`BackendFactory::virtual_path`
  让 `path` 只作挂载名（配置校验不再 stat 本机目录）。
- 全部 libgfapi 调用在 offload 池（`kHeavy` 归 init/fsync/fallocate/copy 类），身份用
  `glfs_setfsuid/gid/groups` 在 offload 线程逐调用设置并复位（Ganesha 同法）。
- **测试**：`tests/gfapi_fake.{hpp,cpp}` 在本地目录上实现同一张函数表（线程局部
  fsuid、16 字节句柄含世代号、xstat 所有权、按 lk-owner 键的 posix 锁表、可注入的
  传输错误），`tests/test_gluster.cpp` 11 个用例跑完 05 §5.9 检查表（P1/P2/ESTALE、
  命名空间、EXCLUSIVE 重放、cookie 分页+富化、匿名/open-state IO、粘性 commit、
  v4.2、原生 access 身份、jukebox 映射、原生锁、配置工厂、停/起与句柄零泄漏）；
  fuzz 目标 `objid_gluster`（句柄解析边界）。真实 libgfapi 11.2 在本机以
  `LD_LIBRARY_PATH` 加载通过（连接失败路径干净）；真实集群验收脚本
  `scripts/accept_gluster.sh`（需要目标环境：本仓库开发机无 root、无 glusterd）。
- **文档边界**：多网关一致性现在有了原生锁下推，但 gluster 无原生 change 计数
  （`change` 仍为 ctime 合成，`change_attr_type = TIME_METADATA`）——跨网关的
  CTO 依赖 ctime 精度；v4 open/deny 状态仍是网关本地（05 §5.6 口径不变，只是锁这一半
  补齐）。锁下推的 LOCKT 探测无法辨认自身持有的锁（见上表）；被删除文件上残留的
  lock glfd 在后端 stop 时统一关闭（`lightnfs_gluster_lock_fds` 可见）。

**第三后端：Lustre ✅ 已完成（2026-09-04）**（`backend/lustre.{hpp,cpp}`，
`backend/llapi.{hpp,cpp}`；06 册 §6.5 映射表逐项落地）。要点：

- **结构**：Lustre 挂载即 POSIX 树，所以 `LustreBackend : LocalBackend`——本地后端去掉
  `final`，开放 `oid_from_fd` / `open_oid` 两个虚函数与受保护的缓存/能力位成员；IO、
  命名空间、fd/O_PATH 缓存、poison、身份模式零复制继承。`LocalObject::open` 现在把
  `kJukebox` 原样透传（其余错误仍降级为 EOPNOTSUPP），v4 OPEN 已释放文件 → DELAY。
- **FID 句柄**（标记字节 4 + 16B `lu_fid`，`objid_lustre` fuzz 目标）：编码取
  `name_to_handle_at` 的 FILEID_LUSTRE 句柄（回落 `LL_IOC_PATH2FID`），解析走
  `<mount>/.lustre/fid/<fid>`——无需 CAP_DAC_READ_SEARCH 即得跨重启稳定句柄，这是
  本地后端在非特权部署下做不到的。挂载根自动探测（同 st_dev 上溯）或 `mount=`。
- **能力位**：kStableHandles、kByteLocks（`LustreLockMgr`：每 (文件, owner) 一个 fd +
  `F_OFD_SETLK/GETLK`，`-o flock` 时 MDS 仲裁；状态层下推链路与 gluster 共用）、
  kJukebox（HSM：数据 fd 打开后 `LL_IOC_HSM_STATE_GET`，HS_RELEASED → 一次
  `LL_IOC_HSM_REQUEST(RESTORE)` + kJukebox，不把 offload/io_uring 线程挂在隐式 restore
  上）；不置 kNativeChange（ctime 合成；`LL_IOC_DATA_VERSION` 每条带一次 OST 往返且只
  覆盖数据）。`pref_read/pref_write` 取导出根默认条带（`lustre.lov`，含复合布局）。
- **绑定**：直接 ioctl 内核客户端，uapi 常量抄自 `lustre_user.h`，**无 liblustreapi
  依赖**；`scripts/check_llapi_abi.sh` 有头文件时 static_assert（CI 接入，无头文件跳过）。
- **测试**：`tests/llapi_fake.cpp`（FID = inode+btime 派生 + O_PATH 钉住，`/proc` 重开；
  HSM/restore/条带旋钮）+ `tests/test_lustre.cpp` 10 例；指标
  `lightnfs_lustre_{jukebox,hsm_checks,hsm_restores}_total` / `lightnfs_lustre_lock_fds`；
  `lightnfs-ctl fdcache/clear-poison` 经 LocalBackend 分支直接适用；真实挂载验收
  `scripts/accept_lustre.sh`（本开发机无 Lustre，待目标环境）。
- **文档边界**：多网关一致性仍缺原生 change（09 册口径更新）；已缓存描述符的文件被
  释放时后续 IO 阻塞在 restore 上（门禁只在打开时刻）；OFD 探测不报告持有者。

**第四后端：CephFS ✅ 已完成（2026-09-04）**（`backend/cephfs.{hpp,cpp}`，
`backend/cephapi.{hpp,cpp}`；06 册 §6.8 映射表逐项落地）。要点：

- **为什么是它**：09 册的多网关一致性需要"kNativeChange + kByteLocks 同时具备"的后端——
  gluster 有锁无 change，lustre 亦然，local 反之。CephFS 两者都有：`stx_version` 是 MDS
  的 change attribute（数据/元数据变更都递增，跨客户端一致 → `change_attr_type =
  MONOTONIC_INCR`），fcntl 锁由 MDS 全局仲裁（`ceph_ll_setlk/getlk`）。
- **结构**：与 gluster 同构（运行时 `dlopen("libcephfs.so.2")` 填 46 项函数表、inode 句柄缓存
  + 匿名 IO Fh 缓存、每 OPEN 一个 Fh、每 (文件, owner) 一个锁 Fh、粘性 poison、offload 全
  覆盖）；身份不是线程局部 fsuid 而是每次调用一个 `UserPerm`；错误码全为负返回值。
- **句柄**：标记字节 5 + 8B ino + 8B snapid（`vinodeno_t`，snapid 取 `stx_dev`），Ceph 不复用
  inode 号故无需世代号；`resolve` = `ceph_ll_lookup_vino`（MDS lookup_ino）；readdirplus 的
  ObjId 直接来自 statx，不取 Inode 引用。fuzz 目标 `objid_cephfs`。
- **能力位**：kStableHandles、kNativeChange、kByteLocks、kJukebox（传输类错误；EBLOCKLISTED
  = 会话被列入黑名单 → 硬 EIO + `lightnfs_cephfs_blocklisted_total`，重试无意义）、kSparseOps
  （Ceph 无 extent 图：SEEK 为 RFC 7862 最小实现）、kCopyRange（网关内 read/write 循环，
  libcephfs 无 copy_file_range）。**不置 kNativeAccess**（无 `ceph_ll_access`；ACCESS 走
  `Object::access` 的 mode 位默认实现，OPEN/变更仍由库以调用者身份判定）。
- **绑定与测试**：`scripts/check_cephapi_abi.sh`（C++ 比对每个成员签名，C 单元核对
  `vinodeno_t`/`ceph_statx` 布局；已对 Ceph 20.2.0 头文件通过，CI 无头文件时跳过）；
  `tests/cephapi_fake.cpp`（UserPerm 身份、永不复用的合成 ino、stx_version 计数、随 Fh 关闭
  释放的锁表、可注入 ENOTCONN/EBLOCKLISTED）+ `tests/test_cephfs.cpp` 12 例（含原生 change
  计数、黑名单映射）；指标 `lightnfs_cephfs_{fdcache,objcache}_*` /
  `lightnfs_cephfs_{jukebox,blocklisted}_total` / `lightnfs_cephfs_lock_fds`；`lightnfs-ctl
  fdcache/clear-poison` 已接线；真实集群验收 `scripts/accept_cephfs.sh`（校验启动日志
  `native-change=true native-locks=true`；本开发机无 Ceph，待目标环境）。
- **接口冻结再检验**：零改动——gluster 期加入的 `LockMgr::release()` 与
  `BackendFactory::virtual_path` 直接复用。
- **文档边界**：ACCESS 只看 mode 位（POSIX ACL 仅在库的 OPEN/变更判定里生效）；copy_range
  吃 offload 线程；v4 open/deny 状态仍是网关本地。

### 5.4 长期观察（维持 09 册口径，补充新证据）

- **RPC-over-TLS（RFC 9289）✅ 已落地**（2026-09-01）：AUTH_SYS-only 曾是最大的部署边界，
  现补上传输层加密+服务器认证这一半。落地点：`transport/tls.{hpp,cpp}`（OpenSSL memory-BIO
  异步封装，socket IO 仍全走 io_uring）、`transport/connection.cpp`（STARTTLS 探测在读循环内
  就地协商）、`transport/record_stream.cpp`（`set_tls` 后 recv/send 走 TLS）、`rpc/dispatch.cpp`
  （`required` 模式对明文 NFS 操作回 AUTH_TOOWEAK）、`core/config.cpp` `[tls]` 段 + 校验、
  `main.cpp` 构建 `TlsContext` 接入两个监听器。CMake `find_package(OpenSSL)` 可选：缺 OpenSSL
  时编译为 stub、非 off 模式配置期即拒，构建矩阵不受影响。测试 `tests/test_tls.cpp`：真实
  OpenSSL 客户端跑通 STARTTLS 握手 + TLS 内 echo、optional 仍服务明文、required 拒明文、
  off 拒探测、`[tls]` 配置解析/校验。仍不做 **RPCSEC_GSS/krb5**（身份层仍 AUTH_SYS）。
- NLM/NSM（v3 锁）：按需；
- pNFS flexfiles、写/目录委托：前提未变（写委托需 CB_GETATTR + 单写者工作负载证据；pNFS 为
  决策 D8 非目标）；
- 多网关一致性：依赖 kNativeChange + kByteLocks 后端（见 §5.3）。CephFS 后端（2026-09-04）
  是第一个两位同时具备的后端；真实的多网关检验待目标集群（`scripts/accept_cephfs.sh`
  先跑单网关验收）。v4 open/deny 状态仍是网关本地。

---

## 6. 结构性重构 ✅ 已完成（2026-08-31）

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

落地说明（2026-08-31）：

- §6.1+§6.3 → `core/mutate.hpp`：`core::MutateGuard`（precheck: readonly → 名字；
  enter: squash → ObjId 排序独占锁 → before 采样；finish: after 采样）与
  `core::ChangeSample`（v3 WCC 与 v4 change_info 的唯一采样源）。v3 全部 9 个写过程、
  v4 的 OPEN(create)/CREATE/REMOVE/RENAME/LINK/SETATTR/WRITE/ALLOCATE/DEALLOCATE/CLONE
  均已接入；COPY 因分片放锁模式保留自管锁（语义不变）。两边 readonly 与名字校验
  统一为 readonly → 名字 → 类型/能力位。
- §6.2 → `core/names.hpp`：`check_component`/`valid_component`/`valid_utf8`，
  v3/v4/mountd 三处接入；v4 保留 UTF-8 校验与 INVAL/BADNAME 映射。
- §6.4 → `core/fs_props.hpp`：caps/limits → `FsProps`（pref 钳位、link/symlink/case
  布尔、固定属性常量），v3 FSINFO/PATHCONF 与 v4 属性编码（`AttrSource.fs`）共用；
  statfs 空间属性两边本就共享 `backend::FsStats`，无需再抽。
- §6.5：`Reactor::op_finished` 已删；`OffloadPool::queue_depth()` 删除并把 overflow
  计入 `stats().depth`（指标 `lightnfs_offload_queue_depth` 语义补全为 admitted+overflow）；
  `StateMgr::client_shard` 已在 §2.6 提交中移除；`state_mgr.cpp` free_stateid 的
  过时注释已按现状重写。
- §6.6：v3 `dispatch_proc` 改为按 `Proc` 索引的成员函数指针跳转表，只读过程
  同步拆分为独立 handler。

---

## 7. 测试与工程化 ✅ 已完成（2026-09-01）

### 7.1 测试缺口 ✅

- **零单测模块** ✅：`tests/test_metrics.cpp`（ExportMetrics、text provider、v3 序列
  与 calls==0 跳过规则、扁平计数、append_histogram span 重载）；
  `tests/test_rpcbind.cpp`（假 portmapper 应答器覆盖请求编码与全部应答解析分支——
  成功/带填充 verifier/短应答/xid 不匹配/MSG_DENIED/accept 失败/bool false；
  `rpcbind_target_port()` 测试缝）；test_ctl.cpp 补齐剩余命令面（`answer_async` 的
  state/expire-client/drc flush、metrics/dump-errors/conns/kill-conn 成功路径/
  fdcache [flush]/clear-poison）与 metrics HTTP 头部契约
  （Content-Type/Content-Length/body= exposition）。
- backend ✅：fd cache 并发压力 + `flush_fd_cache` + `/proc/self/fd` 无泄漏回归
  （`Backend.FdCacheConcurrentStressFlushAndNoFdLeak`）；kernel-handle 模式
  （强制 kKernel：无特权/不支持的 fs 断言 create() 拒绝，特权环境断言
  kStableHandles + ObjId 跨重启稳定 + resolve）；identity strict 端到端拒绝外来
  cred，setfsuid 无特权降级语义（root 下自跳过，特权路径归 VM 验收）；poison 语义
  已有单测，另经真实注入 fsync EIO 触发（见下）。
- v4.2 边界 ✅：同文件 COPY（同对象单锁分支 + 重叠区间 + ca_count==0）、
  DEALLOCATE 全超 EOF/跨 EOF/恰在 EOF、`max_filesize` 边界（memory 后端补
  `set_max_filesize` 保真旋钮并强制 EFBIG，引擎级断言 WRITE/ALLOCATE 越界回
  FBIG）；CLONE 非块对齐 EINVAL 透传（`BackendWrite.CloneMisalignedEinvalPassthrough`，
  依赖 reflink fs，非 reflink 环境自跳过）。
- 并发/长稳 ✅：§1.2 的 TSAN 用例此前已有（`StateMgr.LockStateidIoRacesParentClose`），
  现由 `scripts/ci.sh` 的 TSAN 腿承载；fd 泄漏回归见上；RSS 级长稳仍归
  `fuzz.sh nightly` / fsx 过夜等 soak 项。
- 故障注入 ✅：`backend/fault.{hpp,cpp}` 统一预算表——fsync EIO（原钩子迁入）、
  写 ENOSPC/EDQUOT、读 EIO、短写（真实写 1 字节，锻炼两个 write 重载的续传环）、
  慢盘（reactor 上 sleep）；env（`LNFS_FAULT_*`，fault_inject.sh 兼容）与
  `fault::arm()/clear()`（进程内可重置，单测可驱动）双入口，`tests/test_fault.cpp`
  覆盖；网络 RST/半关闭在 `test_transport_loopback.cpp`
  （中途 RST 后服务器保持健康、半关闭仍排空流水线应答、记录中途半关闭走 EBADMSG）。

### 7.2 fuzz 扩面 ✅

6 个 target（`fuzz/fuzz_{handle_request,file_handle,config,record_stream,v4_attrs,
objid_local}.cpp`）：file_handle 解码（含对带正确 HMAC 句柄的定向突变）、
`LocalBackend::open_oid`（原始字节 + 对活 fallback ObjId 的突变，触达反查表命中与
re-verify）、TOML 解析器、record_stream 分片重组（FakeRing 按输入自导的 1–16 字节
完成粒度投喂）、v4 attrs bitmap/settable-fattr。种子语料入库
（`fuzz/seed/<target>/`，原 17 个语料转正为 handle_request 种子）、字典
（`fuzz/dict/*.dict`）、`scripts/fuzz.sh`（smoke 120s / nightly 1h / run / minimize
（-merge）/ regress）。hermetic：state_dir 改为 $TMPDIR 下 mkdtemp、退出清理；
无 clang 构建下每 target 一个 `fuzz_regress_<t>` 进 ctest 回放种子 + 本机语料
（旧的 ctest 注册不带参数、从不回放语料的问题一并修正）。

### 7.3 工程化 ✅

- `scripts/ci.sh` 一站承载：GCC/Clang × Debug/Release × ASAN+UBSAN × TSAN × epoll ×
  fuzz（regress + smoke）+ errmap/format 门禁；`quick`/`full`/`nightly`（nightly 追加
  bench 门禁与 1h fuzz）；并行度默认半数核（`LNFS_JOBS` 可调）。
  `gen_errmap_cases.py --check` 注册进 ctest（`errmap_check`）；`bench_gate.sh` 经
  `LNFS_ENABLE_BENCH_GATE=ON` 注册（默认关：需 Release 与安静机器）。README（中英）
  的矩阵/fuzz 节奏/语料表述改为与脚本一致。
- 外部依赖固定 commit ✅：`fetch_{cthon,fsx,pynfs}.sh` 按钉定 SHA 浅取
  （GitHub 镜像优先 + 上游回退），`LNFS_FETCH_HEAD=1` 用于升 pin。
- 孤儿清理 ✅：`gen_dataset.sh` 删除（其 M1 数据集验收入口已随发布清理移除）；
  `docs/test-report.md` 的 `pynfs_m4_expected.txt` 悬空引用改为注明随 M4 脚本移除。
- lint ✅：`scripts/format_check.sh`（clang-format 门禁，`--fix` 可就地改写，工具
  缺失时跳过）；clang-tidy 评估通过并接入——`.clang-tidy`
  （bugprone/performance/concurrency，无风格噪声）+ `scripts/tidy.sh`；覆盖率评估
  通过并接入——`scripts/coverage.sh`（clang 源级覆盖 + llvm-cov，按需报告不设门禁）。

---

## 8. 分阶段执行建议

| 阶段 | 主题 | 内容 | 验收 |
|---|---|---|---|
| v1.1 | 缺陷修复 | §1 全部（P0 一周内、P1 一月内），每项带回归测试 | TSAN/ASAN 全绿；新增回归全过 |
| v1.2 | 热路径性能 | §2.1–§2.4（resolve 缓存、post 快路径、uring flags、writev/编码拷贝、buffer pool） | `bench fullpath` 与 fio（vers=3/4.2）对 baseline 的提升入库为新 baseline |
| v1.3 | 可观测性 + 运维 | §3 全部；§4.1 第一步热重载 + §4.2 ctl 扩展 | p99 可从 Prometheus 算出；reload 不中断 IO 的验收用例 |
| v1.4 | 协议短板 | §5.1（READ_PLUS、cookieverf、xattr 应答语义、open()） | pynfs 对应组通过；稀疏读基准 |
| v2.0 | 委托 + 回传通道 | §5.2；穿插 §6 重构 | CB_RECALL/CB_NOTIFY_LOCK 真实客户端验收；委托削减 GETATTR 的量化 |
| v2.x ✅ | 第二后端 | §5.3，先补能力位链路 | 09 册"接口冻结检验"的 DoD：接口零改动（仅加可选 `LockMgr::release()` + `BackendFactory::virtual_path`）跑通 GlusterFS；真实集群验收待目标环境（`scripts/accept_gluster.sh`） |

测试与工程化（§7）不单列阶段，按"谁改到谁补齐"原则摊入各阶段，v1.1 优先补
fuzz 扩面与故障注入钩子。
