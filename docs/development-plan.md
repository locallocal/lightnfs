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

- [x] 自动化验收脚本（VM/容器内真实 mount）：`mount -o vers=3` 后 `ls -lR`、`cat`、`md5sum -c` 全通过
      —— 脚本族交付并接入 CI：`gen_dataset.sh`（数据集+manifest）、`accept_m1.sh`（挂载内校验）、
      `accept_m1_vm.sh`（root VM 一键，CI `m1-acceptance` 作业即真实 mount）、`accept_m1_container.sh`
      （docker 特权容器一键）。本机开发环境无特权，另以 `lnfs_accept_client`（用户态 NFSv3 客户端，
      真实 TCP 打真实 lightnfsd）完成等价校验：全树遍历逐字节比对（= ls -lR/cat/md5sum）+ 负路径
      （伪造句柄 BADHANDLE、坏 cookieverf、11 个写过程 PROC_UNAVAIL、只读 ACCESS 掩码）
- [x] cthon basic 只读部分通过 —— 只读可运行子集定义为 test3（lookup）/test5b（read）/test9（statfs），
      配合 `-n` + 预置目录树；test5b 尾部 unlink 清理以 `CTHON_RO=1` 补丁跳过（`fetch_cthon.sh` 自动
      应用）。子集已在写保护目录验证全过并由 `cthon_ro.sh` 挂入 mount 验收与 CI；其余 basic 测试均含
      写操作，归属阶段 2 §4.6
- [x] 大目录（10 万项）readdir 遍历正确；并发读压测无泄漏（ASAN 长跑）—— 本机执行通过，见 3.6

**阶段 1 DoD**：上述验收全过（真实 mount 路径以 CI `m1-acceptance` 作业为常态回归）；接口评审结论
归档（映射表勾选记录进 docs）。

### 3.6 M1 验收完成记录（2026-08-22）

- 工具：`tests/accept_client.cpp`（`lnfs_accept_client`）—— 用户态 NFSv3/MOUNTv3 客户端，
  walk（READDIRPLUS 递归遍历 + READ 逐字节比对后端目录 + READLINK/ACCESS/FSSTAT/FSINFO/PATHCONF +
  负路径检查）、bigdir（纯 READDIR 全分页，唯一性+完整性）、stress（多连接流水线随机偏移 READ，
  应答逐字节校验）；无 root 依赖，兼作阶段 2 起的每 PR 协议回归
- 数据集（`gen_dataset.sh`）：块边界尺寸族（0/1/511/4095/4096/4097/64K±1/1M+3/8M）、深目录、
  unicode/空格名、相对/绝对/悬空符号链接、10 万项平铺目录、cthon 预置树、md5 manifest
- 本机结果（Release，io_uring，回环）：walk 8 目录 100016 文件（10.8MB 逐字节校验）+ 3 符号链接
  全过；bigdir 10 万项全分页 0.23s 无重复无遗漏；stress 8 连接 ×32 流水线 30s 完成 1274 万次读
  （约 42.5 万 ops/s）全部校验通过
- ASAN+UBSAN（Debug）：walk/bigdir 同样全过；stress 8×32 长跑 300s 完成 1929 万次读
  （约 6.4 万 ops/s，全部逐字节校验）后平滑退出，LeakSanitizer 无泄漏、无 sanitizer 报错
- CI：新增 `m1-acceptance` 作业 —— runner VM 内先跑回环验收（含 ASAN 60s 短稳），再
  `accept_m1_vm.sh` 真实 `mount -o vers=3` + `ls -lR`/`cat`/`md5sum -c` + bigdir 计数 + 只读
  强制 + cthon 只读子集
- 偏差记录：cthon basic 的"只读部分"按上（3.5）定义为 test3/test5b/test9 子集；test5b 打
  `CTHON_RO` 跳过尾部 unlink 清理（补丁最小、上游其余零改动）。§10 风险表"验收环境成本"要求的
  mount/cthon 一键框架已随本节交付，阶段 2 fsx/cthon 全量可直接复用

---

## 4. 阶段 2：v3 读写生产化（=M2+M3）

> 设计依据：[03-transport-rpc-xdr.md](design/03-transport-rpc-xdr.md) §3.7、[04-nfs-core.md](design/04-nfs-core.md) §4.2/4.4、[06-backends.md](design/06-backends.md) §6.2–6.4、[08-config-observability.md](design/08-config-observability.md)

### 4.1 写路径与创建族（第 1–3 周）

- [x] backend_local：write 三档稳定级（Unstable=pwrite/DataSync=pwrite+fdatasync/FileSync=pwrite+fsync）、commit=fdatasync、**fsync EIO 后该文件 commit 恒错**（poison 集合，不吞 writeback 错误）
- [x] backend_local：create（含 EXCLUSIVE verifier 存 atime/mtime + EEXIST 重放比对，跨进程重启契约单测）、mkdir、symlink、mknod、unlink、rmdir、rename（renameat2）、link、setattr；MemoryBackend 同步补齐写路径（引擎单测与全链路基准用）
- [x] core::mutate 模板：exclusive 锁→before→后端 op→after；RENAME/LINK 双对象按 ObjId 排序取锁；失败路径同样采样 after
- [x] core 权限层：协议层预检（ROFS、mode 位快判、属主放宽惯例、跨导出 XDEV 拦截）+ 后端 EACCES/EPERM 权威
- [x] v3 引擎：WRITE/COMMIT（boot verifier）、SETATTR（含 guard ctime→NOT_SYNC）、CREATE 三模式（UNCHECKED 对已存在文件应用 size）、MKDIR/SYMLINK/MKNOD（BADTYPE 校验）/REMOVE/RMDIR/RENAME/LINK——21 个过程补齐；errmap 白名单按 RFC 1813 全过程展开 + 对照单测
- [x] WCC 全过程覆盖（含失败分支 post_op_attr；wire 级单测断言 pre/post 存在性）

### 4.2 DRC 与可靠性（第 2–4 周）

- [x] DRC（Sharded，key 含 peer/xid/proc/args 前 256B 校验和）：kMiss/kInProgress（AsyncCondVar 等待原应答，绝不并发重执行）/kDone 三态；仅 9 个非幂等过程缓存；完成序 FIFO+TTL+内存上限淘汰；执行异常/GARBAGE 路径 abort 唤醒等待者重执行（偏差：非幂等应答一律缓存不回退——应答均为小体，宁可缓存的分支即设计推荐路径）
- [x] boot epoch 持久化（state_dir/boot_epoch，写+fsync+rename，每次启动 +1）→ write verifier
- [x] 崩溃恢复用例：`accept_client crash-write/crash-recover` + 脚本 kill -9 循环——verifier 跨重启变化、4MiB unstable 数据重发后逐字节收敛（Release 与 ASAN 均实测通过）

### 4.3 fd 缓存完备与身份执行（第 3–4 周）

- [x] FdCache 完备：容量/水位驱逐、O_RDONLY→O_RDWR 就地升级复用（旧引用经 shared_ptr 退役）、只读降级路径、命中/升级/驱逐统计（ctl 可查）
- [x] 身份模式 1（权限位自查，默认）+ `identity="strict"`（faccessat2(AT_EACCESS)+fsuid 切换复核）+ 模式 2 `identity="setfsuid"`（offload 线程级 fsuid/fsgid 切换，附组不切换为文档化限制）；压测对比需 root 环境，随 VM 验收脚本执行（本机无特权仅功能验证）

### 4.4 可观测性与工具最小版（第 4–6 周）

- [x] obs：异步日志（调用点栈上定长 format_to_n 零分配 → 环形槽 → 落盘线程，满环丢弃计数）、级别约定落地
- [x] 每请求摘要行（debug：proc/xid/peer/status）+ 错误应答环形采样（64 条，dump-errors 取出）
- [x] Prometheus 文本口：rpc（每过程 calls/errors/duration）/transport（连接、拒绝、背压）/io 字节/drc/fd 缓存指标组；`[server] metrics_port` HTTP 端点 + ctl 快照双通道（runtime 组随 per-reactor 池拆分补充）
- [x] `lightnfs-ctl` 最小版（unix socket）：ping/metrics/dump-errors/drc/fdcache
- [x] `lightnfs-fh` 句柄解码工具（hex→ver/fsid/ObjId + `--key` HMAC 校验，真实句柄实测）

### 4.5 安全清单部分落地（第 5–6 周）

- [x] 08 分册 §8.5 的 1–4、6 项落地，逐项实现点与验证手段归档于 [security-checklist.md](security-checklist.md)（5 随阶段 4，7/8 随阶段 5）

### 4.6 验收（第 6–8 周）

- [x] cthon04 basic/general/special 全过（vers=3）—— `accept_m2_vm.sh` 一键（拉取/构建 cthon04 + rw 挂载 + 三套件全量），接入 CI `m2-acceptance` 作业在 runner VM 内真实 mount 执行；本机无特权环境以 `accept_client wtest` 完成协议级等价校验（全部写过程逐字节比对后端目录）
- [x] fsx（`fetch_fsx.sh` 独立构建 xfstests fsx，本机 5000 ops 冒烟通过）——每 PR 5 万 ops 入 CI；**≥12h 过夜跑**为同脚本传大 FSX_OPS 的日历项（nightly 任务，与 24h fuzz 同批），首轮过夜记录待 CI 环境执行
- [x] kill -9 网关重启：verifier 语义验证脚本（crash-write/recover）本机实测通过；VM 脚本另含同一挂载免 remount 的无感恢复检查
- [x] 并发压测：10000 连接全部存活并各自应答 + 单连接 512 深流水线（8×inflight 上限）全部应答（背压路径验证）；超限连接被拒并计数

**阶段 2 DoD**：v3 可对外试用（受信网络）；此后 v3 行为进入回归保护（cthon+fsx 经 `m2-acceptance` 周期跑）。

### 4.7 阶段 2 完成记录（2026-08-22）

- 测试：86 个单测/集成测试（较 M1 +12），Debug/Release/ASAN+UBSAN/TSAN 四配置全绿；新增引擎 wire 级
  写路径测试（WCC/guard/EXCLUSIVE 重放/ROFS/BADTYPE/DRC 字节级重放/errmap 白名单）与
  本地后端写契约测试（稳定级、verifier 跨重启、命名空间操作、fd 升级、EIO poison）；
  顺带修复 test_backend.cpp 既有 run_runtime 辅助的 notify-after-unlock 竞态（TSAN 揪出）
- 回环验收（`accept_m2_local.sh`，Release+ASAN 实测全过）：wtest（三稳定级写 4MiB+ 逐字节
  校验、DRC 线上重传字节级一致）；kill -9 崩溃恢复（epoch 1→2，数据重发收敛）；10k 连接
  风暴 + 512 深流水线背压；ctl/metrics/fh 工具链冒烟；ASAN 60s 读压测泄漏零报告
- 基准：补齐 02 §2.8 第三层 `bench_fullpath`（阶段 0 遗留项）——单 reactor GETATTR
  ~31.4 万 rps、READ-4k ~26.4 万 rps（Release，io_uring，回环）
- 结构性偏差：DRC 大应答回退规则按"宁可缓存"分支实现（见 4.2）；Prometheus runtime 指标组
  与 buffer 池配置化随 per-reactor 池拆分（阶段 3 前）；identity=setfsuid 的吞吐压测对比
  需 root，归入 VM 验收执行项；`lightnfs-ctl` 的状态表 dump/强制回收命令属阶段 4（7.8）

---

## 5. 阶段 3：v4.1 只读（=M5）

> 设计依据：[04-nfs-core.md](design/04-nfs-core.md) §4.5、[07-state-management.md](design/07-state-management.md) §7.1–7.3/7.5
>
> 可与阶段 2 的 4.4–4.6 并行启动（依赖阶段 1 的 core/backend 与阶段 2 的 boot epoch 持久化）。

### 5.1 v4 引擎骨架（第 1–3 周）

- [x] `nfs4_types.hpp` 骨架清单所需类型（op/status/stateid/bitmap/chan_attrs）+ round-trip 单测；fuzz 经 `handle_request` 全入口自动覆盖（prog=100003 vers=4 语料随 CI 短跑累积）
- [x] COMPOUND 解释器：遇错即停、CFH/SFH 上下文、minorversion=0 恒拒（MINOR_VERS_MISMATCH）、应答大小预算（READ/READDIR 预裁剪 + 其余 op 暂存缓冲，超限替换为 REP_TOO_BIG/REP_TOO_BIG_TO_CACHE）、solo-op 纪律（sessionless 多 op → NOT_ONLY_OP；会话内 BIND_CONN 禁、DESTROY_SESSION 须为末位）、未知 op → OP_ILLEGAL
- [x] bitmap 属性层：单表驱动 GETATTR/READDIR，13 个强制属性 + Linux 客户端实际消费集（mode/owner 数字串/numlinks/times/limits/space/mounted_on_fileid）；statfs 类属性按位预取
- [x] 只读 op 集全部实现；**偏差（超计划）**：真实 Linux 客户端 `cat` 必经 OPEN→READ→CLOSE，故提前实现最小 open-state（CLAIM_NULL/FH 只读 OPEN + CLOSE + stateid 表 + READ 校验 stateid↔对象一致），FREE/TEST_STATEID 一并可用；其余 op NOTSUPP
- [x] errmap `to_v4` 白名单表（RFC 8881 §15.2 按已实现 op 展开）+ 对照单测

### 5.2 伪根与导出集成（第 2–3 周）

- [x] core 内置伪文件系统（fsid=0 保留、按导出路径合成只读前缀树、不经后端）；伪根任意来源可浏览、LOOKUP 跨入导出时校验 CIDR；READDIR 越界子项按导出根属性呈现（fsid 切换 + mounted_on_fileid）；LOOKUPP 从导出根回到伪树；嵌套导出在 v4 命名空间以外层为准（文档化限制）

### 5.3 StateMgr：client/session 部分（第 3–5 周）

- [x] ClientTable/SessionTable（Sharded）；EXCHANGE_ID 完整 RFC §18.35 记录语义（confirmed/unconfirmed 双记录、principal 冲突 CLID_INUSE、无状态 case-3 替换、客户端重启检测、UPD_CONFIRMED_REC_A 更新路径、CONFIRMED_R 回旗）；CREATE_SESSION（clientid+seq 重放保护、通道参数钳制、TOOSMALL 下限、GSS 回传凭证解析）；DESTROY_SESSION（连接绑定校验 CONN_NOT_BOUND）/DESTROY_CLIENTID（BUSY 判定）/BIND_CONN_TO_SESSION
- [x] SEQUENCE 快路径：sessionid 查表→槽边界（BADSLOT/BAD_HIGH_SLOT）→seq 三分支 + in-flight 重复 AsyncCondVar 等待；槽缓存（默认 32 槽 ×8KiB 钳制，cachethis 全程按缓存预算裁剪故恒可缓存；缓存下限不足 → REP_TOO_BIG_TO_CACHE 且不消耗槽序）
- [x] 租约续期无锁化（ClientRec.lease_expiry atomic coarse 秒，SEQUENCE 仅 store）；锁序规约以"单分片持锁 + 不跨锁"实现（结构性满足 ①②③④）+ 并发死锁矩阵单测（12 客户端 ×2 reactor 全生命周期风暴）
- [x] 持久化：clients/<hash> 名单（CREATE_SESSION 确认后写入，fsync）；启动 grace 骨架（读放行、CLAIM_PREVIOUS 门禁 NO_GRACE/RECLAIM_BAD、RECLAIM_COMPLETE 全到齐提前出 grace、one_fs=TRUE 不置全局旗）——跨"重启"单测覆盖

### 5.4 验收（第 5–6 周）

- [x] `mount -o vers=4.1,ro` 挂载/浏览/读全正常 —— `accept_m3_vm.sh` 一键（真实 mount + ls -lR/cat/md5sum -c + 只读强制），接入 CI `m3-acceptance` 在 runner VM 执行；本机无特权以 `accept_client v4walk`（用户态 4.1 客户端：会话建立、伪根穿越、OPEN/READ/CLOSE 逐字节校验、槽重放字节级一致、minorversion=0 拒绝）实测通过
- [x] pynfs 4.1 会话组通过（CI 集成随 `m3-acceptance` 持续跑）—— 本机实测：exchange_id/create_session/destroy_session/destroy_clientid/reclaim_complete 五组 **67 通过 / 8 失败，8 个失败全部为需要写支持的 OPEN(CREATE) 用例（阶段 4 范围）**；排除项：EID9（租约到期扫描，6.2 交付）、EID50（SP4_SSV，设计不支持）
- [x] v3/v4 双挂载并发读一致 —— 本机：同一后端树经 v3 `walk` 与 v4.1 `v4walk` 双客户端逐字节校验一致；VM 脚本另做真实双挂载并发 cat + `diff -r` + 双 md5 校验（4.8 锚点）

### 5.5 阶段 3 完成记录（2026-08-22）

- 测试：97 个单测/集成测试四配置全绿（Debug/Release/ASAN+UBSAN/TSAN）；新增 v4 wire 级
  测试（COMPOUND 纪律/会话建立/槽重放字节一致/伪根 fsid 穿越/特殊与坏 stateid/READDIR
  预算分页/errmap）与 StateMgr 并发矩阵、grace 跨重启单测
- pynfs（用户态直连，无需 mount）：会话五组 67/75 通过（8 个失败全为写依赖）；扩展组
  sequence/lookup/lookupp/putfh/compound/secinfo_no_name 37/44 通过（余 4 为无特权环境
  无法 mknod 的块/字符设备树对象、3 为写依赖）；会话语义按 pynfs 逐项校准（solo-op 纪律、
  principal 碰撞、case-3 替换、CONN_NOT_BOUND、TOOSMALL 下限、REQ/REP_TOO_BIG(_TO_CACHE)
  预算、**槽重放以新 xid 重组 RPC 头**（RFC §2.10.6.2 的 NFS 级契约）、LOOKUP 空名 INVAL、
  LOOKUPP 符号链接 SYMLINK、tag UTF-8 校验、非法 opcode 优先于会话纪律、one_fs 语义）
- 结构性偏差：最小 open-state 提前到本阶段（Linux 客户端读路径硬依赖）；REP_TOO_BIG 预算
  经"小 op 暂存缓冲"实现（READ/READDIR 仍零拷贝直编）；SECINFO(带名)延后（NO_NAME 已实现，
  4.1 客户端以 NO_NAME 为主）；`sequence` 组含租约长睡眠用例随 nightly 跑不阻塞每 PR

---

## 6. 阶段 4：v4.1 读写 + 状态全量（=M6）

> 设计依据：[07-state-management.md](design/07-state-management.md) 全篇

### 6.1 状态表全量（第 1–4 周）

- [x] StateTable/FileStateIdx；stateid.other 含 boot_epoch（STALE_STATEID 零成本判定）——`states_`（other→StateRec）+ `files_`（{fsid,oid}→opens）双索引 + `ClientRec.states` 反引用，16 分片、单分片持锁、持锁零后端 IO（并发矩阵单测仍为死锁自由证明）
- [x] OPEN（claim NULL/FH/PREVIOUS）：share reservation 冲突裁决（SHARE_DENIED，匿名 stateid 受 deny 约束 → LOCKED）、同 owner 合并（并集 access/deny、seqid++）、backend OpenPtr 入状态表；create UNCHECKED/GUARDED/EXCLUSIVE4/EXCLUSIVE4_1（attrset 回报）、POSIX 权限预检、委托意愿 → OPEN_DELEGATE_NONE_EXT 带原因；**补齐**：当前 stateid（{1,0}）语义（OPEN/DOWNGRADE/CLOSE 置、SAVEFH/RESTOREFH 随句柄保存、PUTFH/LOOKUP 等清除）、未 RECLAIM_COMPLETE 的客户端建状态 → GRACE（RFC §18.51.3）
- [x] CLOSE/OPEN_DOWNGRADE；OpenPtr 释放移出临界区（状态管理器内所有释放均在最后一把分片锁之后）；seqid 纪律：0=当前、旧 OLD_STATEID、超前 BAD_STATEID；DOWNGRADE 仅收窄（否则 INVAL）
- [x] READ/WRITE/COMMIT/SETATTR 带 stateid：校验顺序（特殊 stateid→查表→client/对象→seqid→OPENMODE）→ 落到与 v3 相同的后端调用；verifier = boot epoch 与 v3 共用；SETATTR fattr4 可设置集 + BADOWNER/INVAL/ATTRNOTSUPP 判定、attrsset 恒编码
- [x] CREATE（DIR/LNK/BLK/CHR/SOCK/FIFO，REG→BADTYPE）、REMOVE/RENAME/LINK 等 v4 名字空间 op 补齐（change_info4 排他锁下 atomic；跨导出 XDEV；伪根 ROFS；非 UTF-8 名 INVAL）；**超计划**：VERIFY/NVERIFY（同掩码编码字节比较）

### 6.2 租约、courtesy 与 grace/reclaim（第 3–6 周）

- [x] LeaseQueue 到期扫描协程（每秒一轮，reactor 0 常驻）；courtesy 状态（冲突即回收 / 超时 `courtesy_multiplier`×lease 无条件回收，默认 24×；courtesy 期内重新 SEQUENCE 即复活）；回收动作链（StateRec→OpenPtr 析构→files_ 反引用→会话销毁→ClientRec→稳定名单）；客户端重启（新 verifier 经 CREATE_SESSION 确认）走同一条链
- [x] grace 完整：CLAIM_PREVIOUS/reclaim 仅限名单（RECLAIM_BAD）、RECLAIM_COMPLETE 之后 reclaim → NO_GRACE、普通建状态→GRACE、匿名 WRITE→GRACE、读放行、RECLAIM_COMPLETE 全到齐提前出 grace
- [x] state 指标组全量（7.8 清单：clients/sessions/opens/files/courtesy/grace 剩余/槽重放/租约到期/三路回收/share_denied/open_merges）；`lightnfs-ctl` 补 `state`（指标 + 三表 dump）与 `expire-client <id>` 强制回收；配置 `[protocol] lease`/`courtesy_multiplier`

### 6.3 验收（第 6–8 周，长稳与开发并行）

- [x] cthon basic/general（vers=4.1）；fsx 过夜 —— `accept_m4_vm.sh`（root VM，CI `m4-acceptance`：cthon b/g/s + fsx，`FSX_OPS=2000000` 为过夜参数）；本机无特权以 `accept_client v4rw`（用户态 4.1 读写客户端：OPEN(CREATE)/分块 UNSTABLE WRITE+COMMIT verifier/逐字节读回与后端比对/SETATTR 截断/双客户端 share deny/DOWNGRADE/CREATE-RENAME-LINK-REMOVE 后端镜像校验）实测通过
- [x] 服务器重启 reclaim 用例（7.5 场景脚本：带打开状态重启→grace 内 reclaim→数据无损）—— `accept_client v4reclaim`（kill -9 + 重启 + 同 owner 重建会话 + CLAIM_PREVIOUS + 旧 stateid STALE/普通 OPEN GRACE/事后 NO_GRACE 门禁）实测通过；VM 脚本以内核客户端持开文件跨重启写入校验；单测 `RestartReclaimWithinGrace`/`GraceReclaimGate` 覆盖
- [x] 租约回收用例（kill 客户端 VM→courtesy→冲突回收与超时回收两路径）—— `accept_client v4courtesy`（持有者断连不 CLOSE：租约内 SHARE_DENIED、到期后冲突回收放行、第二持有者超时回收）+ `lightnfs-ctl state` 计数校验实测通过（lease=3s）；单测 `CourtesyConflictAndTimeoutReclaim`
- [x] v3/v4 混布：同后端 v3 写 + v4 状态并发（文档明示的边界行为验证）—— VM 脚本双挂载并发写互见；本机 v3 `walk` 与 v4 `v4walk`/`v4rw` 同后端交叉回归

### 6.4 阶段 4 完成记录（2026-08-22）

- 测试：107 个单测/集成测试三配置全绿（Release/ASAN+UBSAN/TSAN）；新增 StateMgr 三组
  （share/merge/downgrade/close 纪律、courtesy 冲突+超时+强制+复活、grace 门禁）与 v4 wire
  七组（create/write/commit/读回、EXCLUSIVE4(_1) 重放与 OPENMODE、share deny/LOCKED/
  DOWNGRADE/CLAIM_FH 合并、SETATTR 各属性与错误码、名字空间 op、跨"重启" reclaim、
  当前 stateid + RECLAIM_COMPLETE 门禁）
- 回环验收（`accept_m4_local.sh`）：v4rw / v4walk / walk / v4reclaim / v4courtesy / ctl 全
  通过；pynfs 4.1（用户态直连）open/rename/verify/courteous/currentstateid + 阶段 3 全部
  组：**184 用例 162 通过 / 22 失败，22 个失败全部为预期排除**（LOCK 依赖 4、委托/回传
  依赖 7、pynfs 自身 NameError 1、需 root 的块/字符设备树对象 10；`scripts/pynfs_m4_expected.txt`）
- 按 pynfs 校准的语义：seqid=0 在 CLOSE/OPEN_DOWNGRADE 同样表示"当前版本"；当前 stateid
  特殊值；未 RECLAIM_COMPLETE 即建状态 → GRACE；非 UTF-8 名 INVAL；OPEN_DELEGATE_NONE_EXT
- 已知边界（详见 [m4-v41-readwrite.md](m4-v41-readwrite.md)）：无特权回退句柄在无
  STATX_BTIME 的文件系统（tmpfs）上句柄不跨重启稳定（`v4reclaim` 打印提示并按名重解析后
  继续校验状态语义；生产用内核句柄/btime 文件系统无此限制）；v3 写不受 v4 deny 约束；
  LOCK/委托/SECINFO(带名)/ACL 属阶段 5/6

---

## 7. 阶段 5：v4 锁与安全完备（=M7）

> 设计依据：[07-state-management.md](design/07-state-management.md) §7.6、[08-config-observability.md](design/08-config-observability.md) §8.5

- [x] 网关内 LockMgr（实现 5.8 接口）：per-ObjId 区间段表、POSIX 合并/拆分/升降级、非阻塞（DENIED+冲突者），`backend::LockMgr` 唯一 v1 实现者——`src/state/lock_mgr.{hpp,cpp}`
- [x] LOCK/LOCKT/LOCKU op + lock stateid（parent_open 关联、lock owner 表、seqid 纪律、OPENMODE/DENIED/OLD_STATEID、CLOSE 连带释放、FREE_STATEID→LOCKS_HELD、courtesy 冲突回收、grace 门禁）——`state_mgr.cpp` 锁状态层 + `engine.cpp` 三 op
- [x] SECINFO/SECINFO_NO_NAME 完整（AUTH_SYS-only 恒 `[AUTH_SYS]`、消费 CFH、名字校验）
- [x] 错误白名单全覆盖复查（4.6 两张表对照调研分册全量过一遍）：v3 CREATE 族收敛 + MKDIR MLINK/REMOVE·RMDIR PERM 文档化偏差；v4 新增 LOCK/SECINFO/FREE_STATEID/RECLAIM_COMPLETE/无参 op 行——`Nfs4.ErrmapV4Whitelist` 扩充对照
- [x] 安全清单收尾：最小特权（`packaging/systemd/lightnfs.service`：CAP_DAC_READ_SEARCH+CAP_NET_BIND_SERVICE、CapBoundingSet 同集、seccomp `@system-service` 去高危集 + io_uring/句柄系统调用白名单，`scripts/gen_seccomp_allowlist.sh` 从真实负载 strace 生成）、部署文档（`docs/deployment.md`：AUTH_SYS 信任边界、TLS/WireGuard 前置、运维、限制）
- [x] **验收**：cthon lock 组（vers=4.1，`accept_m5_vm.sh` -l + 内核 fcntl 字节锁）；pynfs 锁相关组（本机 currentstateid/courteous/secinfo_no_name/SEC 26 通过/1 失败=CSID7 pynfs 自身 NameError）；8.5 全 8 项逐条验收记录归档（`security-checklist.md`）

### 7.1 阶段 5 完成记录（2026-08-22）

- 测试：111 个单测/集成测试三配置全绿（Release/ASAN+UBSAN/TSAN）；新增 `LockMgr`
  （POSIX 合并/拆分/升级/冲突/区间助手 2 组）、`StateMgr.ByteRangeLocksLifecycle`
  （LOCK new/existing owner、DENIED 持有者、LOCKT、lock stateid IO 模式、FREE/LOCKU/CLOSE、
  courtesy 冲突回收）、`Nfs4.LockOpsAndSecinfo`（wire 级 LOCK/LOCKT/LOCKU/SECINFO）
- 回环验收（`accept_m5_local.sh`）：`v4lock`（双客户端锁全流程）+ v4rw/reclaim/courtesy/ctl
  回归 + pynfs 锁/secinfo/courtesy 组；两配置（Release/ASAN）通过
- 此阶段结束 = **v1 发布候选**：Backend API 接口 `native_locks()` 切换点已定型（网关内
  LockMgr 为唯一 v1 实现者）；部署/运维/限制文档齐备

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
