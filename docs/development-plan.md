# lightnfs 开发计划

本计划依据 [docs/design/](design/README.md) 设计文档制定，将 [09-roadmap.md](design/09-roadmap.md) 的阶段 0–6 展开为可执行的工作分解（WBS）、依赖关系、测试基建与验收清单。协议语义细节以 [nfsv3/](nfsv3/README.md)、[nfsv4/](nfsv4/README.md) 调研分册为准，此处只引用。

**工期基准**：按 1–2 名全职开发者估算，单位为周；多人并行时可按依赖图压缩。估算含单测与联调，不含长稳测试的日历等待时间（fsx 过夜等可与后续开发并行）。

---

## 1. 总览

| 阶段 | 内容 | 对应里程碑 | 估算 | 关键出口（DoD） |
|------|------|-----------|------|----------------|
| 0 | 工程基建 + 协程运行时 + XDR/传输 | — | 6–8 周 | null-RPC 单 reactor ≥100k rps；TSAN/ASAN 全绿 |
| 1 | Backend API 定稿 + v3 只读 | M1 | 5–6 周 | Linux 客户端 vers=3 只读全通过；cthon basic 只读通过 |
| 2 | v3 读写生产化 | M2+M3 | 6–8 周 | cthon04 basic/general/special 全过；fsx 过夜；kill -9 重启无感恢复 |
| 3 | v4.1 只读 | M5 | 6–8 周 | vers=4.1,ro 挂载正常；pynfs 4.1 会话组通过；v3/v4 双挂载读一致 |
| 4 | v4.1 读写 + 状态管理 | M6 | 8–10 周 | cthon（vers=4.1）；fsx 过夜；重启 reclaim、租约回收用例通过 |
| 5 | v4 锁 + 安全完备 | M7 | 4–6 周 | cthon lock 组；pynfs 锁组；8.5 安全清单全项验收 |
| 6 | v4.2 甜点 + 第二后端 | M8 | 按需 | 接口冻结检验：第二后端零接口改动接入 |

依赖主线：**阶段 0 → 1 → 2** 与 **阶段 0 → 3 → 4 → 5** 两条链；阶段 3 只依赖阶段 1 的 core/backend 产出，可在阶段 2 进行中由第二人并行启动。

---

## 2. 阶段 0：工程基建与运行时（无协议）

> 设计依据：[02-runtime-concurrency.md](design/02-runtime-concurrency.md)、[03-transport-rpc-xdr.md](design/03-transport-rpc-xdr.md) §3.1–3.3、3.6

### 2.1 工程骨架（第 1 周）✅

- [x] 仓库布局按 01 分册 §1.7 建 `src/` 目录树；CMake（C++20，GCC/Clang 双工具链）
- [x] 三方依赖引入：liburing（系统包优先，`scripts/fetch_liburing.sh` vendor 兜底）、libFuzzer。
      偏差：tl::expected → 自研 `lnfs::Result<T>`（同形 API，util/result.hpp，后续可平替）；
      GoogleTest → 自研 mini_test（tests/mini_test.hpp，宏兼容便于日后迁移）；toml++ 延后到配置落地（阶段 1/2）
- [x] CI 流水线（.github/workflows/ci.yml）：Debug/Release/ASAN/TSAN 四配置 + ctest + 短时 fuzz + clang-format（阶段 0 内 format 为警告不阻断；clang-tidy 待接入）
- [x] `util/`：`Result<T>`、`Errno` 强类型（含 kJukebox/kGarbage/kEof 哨兵）、`Flags<E>`、SmallVec、日志雏形

### 2.2 协程运行时 `runtime/`（第 2–4 周）✅

- [x] `Task<T>`（惰性、单消费者、移动消费、对称转移）+ `spawn`/`spawn_on`；spawn 任务未捕获异常 fail-fast abort
- [x] `Reactor`：ring 主循环、CQE→协程恢复、定时器（偏差：最小堆实现，接口同 TimerWheel，量级不足前不换）、MpscQueue 远程唤醒、可注入时钟（测试用）
- [x] `RingOps` 抽象层 + FakeRing（src/runtime/testing/，时序穿插、EINTR/短读/乱序完成/取消注入）——单测全走 fake
- [x] uring 封装原语：read/write/fsync/recv/sendv/accept/statx/openat（+close/cancel_fd），负 errno 约定
- [x] `offload()` 池（唯一跨线程点，完成后回原 reactor；异常跨线程传播）
- [x] 同步原语：AsyncMutex（FIFO、所有权直接移交）、AsyncSharedMutex（FIFO 防写者饥饿）、AsyncCondVar、Semaphore、Event、`Sharded<T,N>`
- [x] 取消模型：CancelToken/CancelSource + `with_timeout`/`sleep_for`；`uring_cancel_fd`（IORING_ASYNC_CANCEL_FD）尽力取消
- [x] epoll 兜底实现（EpollRing：socket 走就绪+非阻塞，文件 IO 走内部 worker）。偏差：两实现常编译，默认 auto 运行时探测 uring 失败自动回退（比纯构建期二选一更利于 CI 矩阵与容器部署）
- [x] Buffer/BufferChain：引用计数定长块、分级池（4K/64K/1M）、水位限制。TODO(阶段1)：拆 per-reactor 池（当前全局线程安全池，接口不变）

**验收 ✅**：fake ring 时序单测全过；ASAN/UBSAN、TSAN 全绿；echo 基准基线建立（见 2.4）。

### 2.3 XDR 与传输层（第 4–6 周，与 2.2 尾部重叠）✅

- [x] `xdr/`：XdrDec（缓冲链游标、opaque 零拷贝 span、跨段自动 gather、平坦 span 模式、越界→kGarbage）/ XdrEnc（raw_gap 预留后填、attach 零拷贝挂接、limit 预算钩子）
- [x] `transport/`：Listener（轮转指派 reactor、可取消 accept）、connection_main（流水线解析、取消→cancel_fd→drain 收敛）、RecordStream（多片段重组、片段/总长上限、per-conn 发送串行化、部分发送续传）
- [x] 连接数上限 + per-peer 限制（ConnTracker）、每连接在途 Semaphore 背压
- [x] `rpc/`：rpc_msg 解析与应答编码、分层错误纪律全覆盖（RPC_MISMATCH/AUTH_ERROR/PROG_UNAVAIL/PROG_MISMATCH/GARBAGE_ARGS/SYSTEM_ERR）、Dispatcher 程序注册分发
- [x] `rpc/auth.cpp`：AUTH_NONE/AUTH_SYS 解析 + Authenticator 插槽注册表（squash 随阶段 1 导出表落地，接口已留注）
- [x] fuzz 骨架：libFuzzer 直喂 `handle_request`（fuzz/fuzz_handle_request.cpp）；非 clang 配置以 fuzz_regress 回放语料进 ctest

### 2.4 三层基准（第 6 周，出口门禁）✅（前两项）

按 02 分册 §2.8 建立并入 CI 回归：

1. [x] echo 服务器（纯传输层，bench/bench_echo.cpp）
2. [x] null-RPC（L2，bench/bench_nullrpc.cpp，**出口指标：单 reactor ≥ 100k rps**，程序退出码即门禁）
3. [ ] 伪后端全链路（L4 以上，后端零延迟）——阶段 1 后补齐

**阶段 0 DoD**：三项基准前两项达标 ✅；四配置 CI 全绿 ✅；fuzz 跑 24h 无 crash（短跑无 crash ✅，24h 长跑随 CI 每日任务累计，遗留项）。

### 2.5 阶段 0 完成记录（2026-08-20）

- 测试：64 个单测/集成测试，Debug/Release/ASAN+UBSAN/TSAN 四配置全绿（`ctest`）；
  覆盖 fake ring 时序注入（EINTR/短读/乱序/取消）、真实 uring 与 epoll 双后端冒烟、
  回环端到端（NULL/echo/50 深度流水线/8 并发连接）
- 基准（本机 Release，单 reactor，io_uring）：
  - null-RPC：**~398k rps**（8 连接 × 64 流水线；Debug 构建亦有 ~178k，目标 100k）✅
  - echo：**~538k records/s**（128B 载荷）
- fuzz：libFuzzer 60s 短跑无 crash（~100k exec/s）；语料随协议消息扩展持续补充
- 结构性偏差（均已在上文条目标注）：自研 Result/mini_test 替代 tl::expected/GoogleTest；
  定时器为最小堆；epoll 兜底为运行时探测回退；buffer 池暂为全局共享。
  以上不影响外部接口，替换点已隔离

---

## 3. 阶段 1：Backend API 定稿 + v3 只读（=M1）

> 设计依据：[05-backend-api.md](design/05-backend-api.md)、[06-backends.md](design/06-backends.md)、[04-nfs-core.md](design/04-nfs-core.md)

### 3.1 Backend API 定稿（第 1 周，**接口评审是硬关口**）

- [x] `backend/api.hpp` 全量落码：ObjId/Attr/SetAttr/Cred/Caps/Object/Backend/OpenCtx/Created/DirPage/LockMgr（占位）
- [x] **接口评审**：以 06 分册 Lustre（§6.5）与 GlusterFS（§6.6）两张映射表为验收材料逐项过；结论归档于 `docs/backend-api-review.md`，接口进入 5.10 演进规则管控
- [x] 工厂注册机制 `LNFS_REGISTER_BACKEND` + registry

### 3.2 backend_local 只读子集（第 1–3 周）

- [x] ObjId 编码：name_to_handle_at 的 kernel fhandle；降级模式（ino+btime/process generation hint）同步实现并以能力/文档明示限制
- [x] `resolve`（open_by_handle_at）、`root`、`getattr`（uring_statx；无内核/UAPI change-cookie 时合成）、`lookup`（O_PATH|O_NOFOLLOW 双保险）、`readlink`、`statfs`、`limits`
- [x] `readdir`：getdents64 offload 批量、cookie=d_off、DirPage 尽力带 attr/oid（可配置关闭 enrichment）
- [x] `read`：FdCache 雏形（分片 LRU，acquire/驱逐只关 fd）+ uring pread
- [x] 后端契约单测：能力条件下的 P1/降级重建行为、P2（删除重建新 ObjId）、并发新增时 readdir cookie 不重复遗漏未变项、getattr 即时性

### 3.3 core 只读子集（第 2–4 周）

- [x] 导出表：TOML 解析 + 启动全量校验（fsid 唯一、路径存在、网段格式，错即拒起）；`check_client` IP 校验
- [x] 文件句柄：编解码（ver/fsid/backend_oid/HMAC-SipHash）、hmac.key 持久化于 state_dir、BADHANDLE/STALE/ACCES 三分支
- [x] Cred 生成 + squash（root/all→anon）在进入后端前一次完成
- [x] ObjLockRegistry（分片 + 引用计数回收）；只读路径使用共享锁
- [x] errmap：`to_v3` 按过程白名单 + 对照单测
- [x] readdir 游标簿记：cookie 0/1/2 保留段、`.`/`..` 合成、+3 偏移换算
- [x] 伪后端（内存文件树）：供全链路基准与引擎单测，不依赖真实磁盘

### 3.4 v3 引擎只读过程 + mountd（第 3–5 周）

- [x] `nfs3_types.hpp` 只读子集类型 + encode/decode round-trip 单测 + fuzz 目标
- [x] 过程：NULL/GETATTR/LOOKUP/ACCESS/READLINK/READ/READDIR/READDIRPLUS/FSSTAT/FSINFO/PATHCONF
- [x] READDIRPLUS 的 dircount/maxcount 双预算控制
- [x] mountd：NULL/MNT/EXPORT（DUMP/UMNT/UMNTALL 空实现）；MNT 走 decode_path→lookup 链→encode_fh
- [x] rpcbind 注册（默认注册系统 rpcbind；`--builtin-portmap` 只读子集延后到阶段 2）
- [x] main/config：启动序（配置→后端 start→监听→注册）、平滑退出骨架

### 3.5 验收（第 5–6 周）

- [ ] 自动化验收脚本（VM/容器内真实 mount）：`mount -o vers=3` 后 `ls -lR`、`cat`、`md5sum -c` 全通过
- [ ] cthon basic 只读部分通过
- [ ] 大目录（10 万项）readdir 遍历正确；并发读压测无泄漏（ASAN 长跑）

**阶段 1 DoD**：上述验收全过；接口评审结论归档（映射表勾选记录进 docs）。

---

## 4. 阶段 2：v3 读写生产化（=M2+M3）

> 设计依据：[03-transport-rpc-xdr.md](design/03-transport-rpc-xdr.md) §3.7、[04-nfs-core.md](design/04-nfs-core.md) §4.2/4.4、[06-backends.md](design/06-backends.md) §6.2–6.4、[08-config-observability.md](design/08-config-observability.md)

### 4.1 写路径与创建族（第 1–3 周）

- [ ] backend_local：write 三档稳定级（Unstable/DataSync/FileSync，RWF_DSYNC 路径）、commit=fdatasync、**fsync EIO 后该文件 commit 恒错**（不吞 writeback 错误）
- [ ] backend_local：create（含 EXCLUSIVE verifier 存 atime/mtime + EEXIST 重放比对）、mkdir、symlink、mknod、unlink、rmdir、rename（renameat2）、link、setattr
- [ ] core::mutate 模板：exclusive 锁→before→后端 op→after；RENAME/LINK 双目录按 ObjId 排序取锁；失败路径同样采样 after
- [ ] core 权限层：协议层预检（ROFS、mode 位快判、属主放宽惯例）+ 后端 EACCES/EPERM 权威
- [ ] v3 引擎：WRITE/COMMIT（boot verifier）、SETATTR（含 guard ctime）、CREATE 三模式、MKDIR/SYMLINK/MKNOD/REMOVE/RMDIR/RENAME/LINK——21 个过程补齐
- [ ] WCC 全过程覆盖（含失败分支 post_op_attr）

### 4.2 DRC 与可靠性（第 2–4 周）

- [ ] DRC（Sharded，key 含 args_checksum）：kMiss/kInProgress（AsyncCondVar 等待原应答）/kDone 三态；仅非幂等过程缓存；LRU+TTL+内存上限；大应答不缓存的回退规则
- [ ] boot epoch 持久化（state_dir/boot_epoch，每次启动 +1）→ write verifier
- [ ] 崩溃恢复用例：kill -9 后重启，客户端重发+verifier 变化触发重写，数据最终一致

### 4.3 fd 缓存完备与身份执行（第 3–4 周）

- [ ] FdCache 完备：容量/水位驱逐、O_RDWR 复用、只读 fs 降级
- [ ] 身份模式 1（权限位自查，默认）+ 可选严格 access()（faccessat2）；模式 2（setfsuid offload）实现并压测对比

### 4.4 可观测性与工具最小版（第 4–6 周）

- [ ] obs：结构化异步日志（per-reactor ring→落盘线程，热路径零分配）、级别约定落地
- [ ] 每请求摘要行（debug）+ 错误应答环形采样
- [ ] Prometheus 文本口：rpc/transport/runtime/backend/drc 指标组
- [ ] `lightnfs-ctl` 最小版（unix socket）：dump-errors、fd 缓存统计、指标快照
- [ ] `lightnfs-fh` 句柄解码工具

### 4.5 安全清单部分落地（第 5–6 周）

08 分册 §8.5 的 1–4、6 项：长度上限逐处校验+fuzz 覆盖、句柄 HMAC+每请求导出校验、名字双层校验（空名/`/`/NUL/`.`/`..`）、squash 单点、资源上限全默认可配。

### 4.6 验收（第 6–8 周）

- [ ] cthon04 basic/general/special 全过（vers=3）
- [ ] fsx 过夜（≥12h）无差异
- [ ] kill -9 网关重启，客户端无感恢复（重发 + verifier 语义验证脚本）
- [ ] 并发压测：万级连接、背压触发路径验证

**阶段 2 DoD**：v3 可对外试用（受信网络）；此后 v3 行为进入回归保护（cthon+fsx 周期跑）。

---

## 5. 阶段 3：v4.1 只读（=M5）

> 设计依据：[04-nfs-core.md](design/04-nfs-core.md) §4.5、[07-state-management.md](design/07-state-management.md) §7.1–7.3/7.5
>
> 可与阶段 2 的 4.4–4.6 并行启动（依赖阶段 1 的 core/backend 与阶段 2 的 boot epoch 持久化）。

### 5.1 v4 引擎骨架（第 1–3 周）

- [ ] `nfs4_types.hpp` 骨架清单所需类型 + round-trip 单测 + fuzz
- [ ] COMPOUND 解释器：ops_table、遇错即停、CFH/SFH 上下文、minorversion=0 恒拒（MINOR_VERS_MISMATCH）、应答大小预算（XdrEnc limit、READDIR 截断、REP_TOO_BIG）
- [ ] bitmap 属性层：`attr_id → {getter, setter?, encoder}` 注册表，getter 从 core Attr 取值
- [ ] 只读 op 集：SEQUENCE/PUTFH/PUTROOTFH/GETFH/LOOKUP/LOOKUPP/GETATTR/ACCESS/READLINK/READ（特殊 stateid）/READDIR/SECINFO_NO_NAME（最小）；其余 NOTSUPP
- [ ] errmap `to_v4` 白名单表（RFC 8881 §15.2）+ 单测

### 5.2 伪根与导出集成（第 2–3 周）

- [ ] core 内置伪文件系统（fsid=0，只读合成目录，不经后端）；伪根任意来源可浏览、进导出时校验 IP

### 5.3 StateMgr：client/session 部分（第 3–5 周）

- [ ] ClientTable/SessionTable（Sharded）；EXCHANGE_ID/CREATE_SESSION/DESTROY_SESSION/DESTROY_CLIENTID/BIND_CONN_TO_SESSION
- [ ] SEQUENCE 快路径：sessionid 查表→槽边界→seq 三分支（new/replay/misordered）+ in-flight 重复等待；槽缓存（32 槽×8KiB，REP_TOO_BIG_TO_CACHE）
- [ ] 租约续期无锁化（atomic coarse 时间戳）；锁序规约 ①②③④ 落地 + 死锁矩阵单测
- [ ] 持久化：clients/ 名单（EXCHANGE_ID 确认后写入）；启动 grace 流程骨架（只读阶段先放行读）

### 5.4 验收（第 5–6 周）

- [ ] `mount -o vers=4.1,ro` 挂载/浏览/读全正常
- [ ] pynfs 4.1 会话组通过（CI 集成 pynfs，此后持续跑）
- [ ] v3/v4 双挂载并发读，后端观察序列与结果一致（4.8 一致性清单锚点脚本）

---

## 6. 阶段 4：v4.1 读写 + 状态全量（=M6）

> 设计依据：[07-state-management.md](design/07-state-management.md) 全篇

### 6.1 状态表全量（第 1–4 周）

- [ ] StateTable/FileStateIdx；stateid.other 含 boot_epoch（STALE_STATEID 零成本判定）
- [ ] OPEN（claim NULL/FH/PREVIOUS）：share reservation 冲突裁决、同 owner 合并（并集 access/deny、seqid++）、backend OpenPtr 入状态表
- [ ] CLOSE/OPEN_DOWNGRADE；OpenPtr 释放移出临界区异步执行
- [ ] READ/WRITE/COMMIT/SETATTR 带 stateid：校验顺序（特殊 stateid→查表→OPENMODE）→ 落到与 v3 相同的后端调用
- [ ] CREATE（目录类对象）、REMOVE/RENAME/LINK 等 v4 名字空间 op 补齐

### 6.2 租约、courtesy 与 grace/reclaim（第 3–6 周）

- [ ] LeaseQueue 到期扫描协程；courtesy 状态（冲突即回收 / 超时 24×lease 无条件回收）；回收动作链（StateRec→OpenPtr 析构→锁表→files_ 反引用→ClientRec→稳定名单）
- [ ] grace 完整：CLAIM_PREVIOUS/reclaim 仅限名单（RECLAIM_BAD）、普通建状态→GRACE、RECLAIM_COMPLETE 全到齐提前出 grace
- [ ] state 指标组全量（7.8 清单）；`lightnfs-ctl` 补状态表 dump、强制回收 client

### 6.3 验收（第 6–8 周，长稳与开发并行）

- [ ] cthon basic/general（vers=4.1）；fsx 过夜
- [ ] 服务器重启 reclaim 用例（7.5 场景脚本：带打开状态重启→grace 内 reclaim→数据无损）
- [ ] 租约回收用例（kill 客户端 VM→courtesy→冲突回收与超时回收两路径）
- [ ] v3/v4 混布：同后端 v3 写 + v4 状态并发（文档明示的边界行为验证）

---

## 7. 阶段 5：v4 锁与安全完备（=M7）

> 设计依据：[07-state-management.md](design/07-state-management.md) §7.6、[08-config-observability.md](design/08-config-observability.md) §8.5

- [ ] 网关内 LockMgr（实现 5.8 接口）：per-ObjId 区间树、POSIX 合并/拆分、非阻塞（DENIED+冲突者）
- [ ] LOCK/LOCKT/LOCKU op + lock stateid（parent_open 关联、lock owner 表）
- [ ] SECINFO/SECINFO_NO_NAME 完整
- [ ] 错误白名单全覆盖复查（4.6 两张表对照调研分册全量过一遍）
- [ ] 安全清单收尾：最小特权（CAP_DAC_READ_SEARCH+CAP_NET_BIND_SERVICE，systemd+seccomp 白名单单元文件）、部署文档（AUTH_SYS 信任边界、TLS/WireGuard 前置）
- [ ] **验收**：cthon lock 组（vers=4.1）；pynfs 锁相关组；8.5 全 8 项逐条验收记录归档

估算 4–6 周。此阶段结束 = **v1 发布候选**：打 tag、发布文档（部署/运维/限制说明）、Backend API 冻结公告。

---

## 8. 阶段 6：甜点与后端扩展（=M8，按需排序）

按价值/成本排序，彼此独立可并行：

1. **v4.2 低成本特性**（~2–3 周）：SEEK/ALLOCATE/DEALLOCATE（kSparseOps）、同步同服 COPY（kCopyRange）、CLONE（kCloneRange）；宣告 minorversion=2；backend_local 对应实现 + 运行时探测
2. **第二后端**（Lustre 或 GlusterFS，~6–8 周）：按 06 分册映射表实现——**接口冻结的真实检验**，出现接口缺口必须走 5.10 演进规则并回写 06 分册
3. **读委托 + 回传通道发送侧**（依赖真实需求触发）：CB_COMPOUND 发送、CB_RECALL、SessionRec.back 启用
4. **可选 NLM/NSM**：仅当 v3 锁刚需出现（nfsv3/06 §6.6 选项 2）

---

## 9. 测试与 CI 基建（贯穿全程）

| 层 | 手段 | 引入时机 | CI 频率 |
|----|------|---------|--------|
| 运行时 | fake ring 时序单测、TSAN/ASAN、frame 哨兵 | 阶段 0 | 每 PR |
| XDR | round-trip 单测、libFuzzer（handle_request 入口） | 阶段 0 起持续加语料 | 每 PR + 每日长跑 |
| 后端契约 | P1/P2、cookie 稳定性、稳定级落盘（可断电模拟盘） | 阶段 1 | 每 PR |
| 错误映射 | 白名单表对照调研分册的生成式单测 | 阶段 1/3 | 每 PR |
| 性能 | 三层基准（echo/null-RPC/伪后端） | 阶段 0 | 每 PR 回归（阈值门禁） |
| 协议一致性 | cthon04、pynfs（4.1）、fsx | 阶段 1 起逐步接入 | 每日（真实 mount 需 VM/特权容器） |
| v3/v4 一致 | 同负载双版本挂载对比（4.8 清单锚点） | 阶段 3 | 每日 |
| 故障注入 | kill -9 重启、客户端 VM kill、fsync EIO 注入 | 阶段 2/4 | 每周 |

CI 环境要点：真实 mount 测试需嵌套 VM 或特权容器（内核 NFS 客户端）；准备两套内核（≥6.6 带 STATX_CHANGE_COOKIE / 老内核 epoll 兜底路径）做矩阵构建。

---

## 10. 风险与应对（登记于 09 分册，此处补执行动作）

| 风险 | 执行动作 |
|------|---------|
| 自研 runtime 正确性 | 阶段 0 独立交付、fake ring 先行、三层基准阈值进 CI 门禁；阶段 1 前不动协议代码 |
| v4 状态机复杂度失控 | 严格按 nfsv4/11.4 骨架清单裁剪，超出清单的 op 一律 NOTSUPP；pynfs 从阶段 3 起每日跑；每请求摘要日志在写第一个 v4 op 前先落地 |
| Backend API 返工 | 阶段 1 第 1 周接口评审为硬关口（两张映射表逐项勾选归档）；此后改动走 5.10 规则 |
| open_by_handle_at 特权依赖 | 降级模式与主路径同期实现（阶段 1），容器 capability 配置写入部署文档 |
| io_uring 内核差异 | epoll 兜底纳入 CI 矩阵；statx/copy_file_range 的 uring 支持运行时探测，探测结果进启动日志 |
| 验收环境成本 | 阶段 1 即搭好 VM 验收脚本框架（mount/cthon/fsx 一键），后续阶段复用 |

---

## 11. 交付物清单（v1 = 阶段 5 末）

- `lightnfsd` 二进制（uring/epoll 双构建）、`lightnfs-ctl`、`lightnfs-fh`
- systemd 单元（seccomp 白名单、最小 capability）
- 配置样例与校验文档（08 分册 §8.1）
- 部署/运维文档：信任边界、降级模式限制、多网关限制（kNativeChange）、grace/重启行为说明
- 测试报告：cthon/pynfs/fsx 通过记录、三层基准数据、8.5 安全清单验收记录
