# M8：甜点与后端扩展（阶段 6）——第 1 项 v4.2 低成本特性

v1 发布候选（阶段 5）之上的"甜点"：按开发计划 §8 的价值/成本排序，本次落地**第 1 项**——
NFSv4.2 稀疏文件三件套（SEEK/ALLOCATE/DEALLOCATE）、同步同服 COPY、CLONE，并宣告
minorversion=2。v3 路径、v4.1 行为、Backend API v1 接口形状均不变（五个后端方法在
05 分册 v1 定稿时已预留，本阶段只是首次被引擎消费）。

§8 其余三项的处置见文末"§8 其余项"。

## 实现要点

### 小版本协商（RFC 7862 §1.4 / 开发计划 D5）

- COMPOUND `minorversion` 接受 1 与 2（0 仍恒 `MINOR_VERS_MISMATCH`，3+ 同样拒绝）。
- 4.2 **不引入新状态**：EXCHANGE_ID/CREATE_SESSION/stateid/租约/grace 与 4.1 完全共用，一个
  会话可以在 minor 1 与 minor 2 的 COMPOUND 间自由切换（`Nfs4.MinorversionTwoOpcodeTable`
  覆盖）。
- 操作码表按小版本收缩：minor 1 下 59–71 回 `OP_ILLEGAL`（4.1 表止于 58）；minor 2 下
  未实现的 4.2 op（IO_ADVISE/READ_PLUS/WRITE_SAME/COPY_NOTIFY/OFFLOAD_*/LAYOUT*）回
  `NOTSUPP`（RFC 7862 §4.2 逐 op OPT 明文允许），72+ 仍 `OP_ILLEGAL`。
  `nfs4_types.hpp`：`kLastOp41`/`last_op_for(minor)`/`minor_supported()`。

### 五个操作（`engine.cpp` "v4.2 sweets" 段）

共同骨架与 READ/WRITE 一致：CFH 必须是导出上的常规文件（伪根/目录 `ISDIR`，其他类型
`WRONG_TYPE`）→ 能力位门禁（后端无对应 `Cap` → `NOTSUPP`，**协议行为随能力位自动收缩**，
05 分册 §5.3 规则）→ 当前 stateid 占位符替换 → `StateMgr::check_io`（special → 表 →
OPENMODE）→ 只读导出 `ROFS` → per-object 锁 → 后端调用 → 白名单错误映射。

| op | 参数校验 | stateid 需求 | 后端 | 结果 |
|----|---------|-------------|------|------|
| SEEK(69) | `what` ∉ {DATA,HOLE} → `UNION_NOTSUPP` | 读 | `seek()`；ENXIO → `NXIO` | `eof`（返回位置 ≥ size，与 knfsd 同）+ offset |
| ALLOCATE(59) | length=0 / 溢出 → `INVAL` | 写 | `allocate()` | 仅状态 |
| DEALLOCATE(62) | 同上 | 写 | `deallocate()` | 仅状态 |
| COPY(60) | `ca_source_server` 非空（跨服）→ `NOTSUPP`；溢出 → `INVAL`；跨导出 → `XDEV` | 源读（SFH）+ 目的写（CFH） | `copy_range()`；count=0 → 至源 EOF（引擎按 getattr 归一） | `write_response4`：无回调 stateid、字节数、`UNSTABLE` + 写验证器（客户端照 WRITE 后 COMMIT）、`cr_consecutive=cr_synchronous=TRUE` |
| CLONE(71) | 溢出 → `INVAL`；count=0 且源偏移 ≥ size → `INVAL`；跨导出 → `XDEV` | 同 COPY | `clone()`；count=0 → 至源 EOF | 仅状态 |

- **异步 COPY 不做**：`ca_synchronous=FALSE` 的请求仍同步完成并回 `cr_synchronous=TRUE`
  （RFC 7862 §15.2.3 允许服务器升级为同步）；无 copy stateid、无 CB_OFFLOAD。
- 两文件 op 的 stateid 占位符：目的走当前 stateid；源若为占位符则取 SAVEFH 随文件句柄一起
  保存的 stateid（无则回退当前）。`{OPEN src; SAVEFH; PUTFH; OPEN dst; COPY(cur,cur)}`
  单 COMPOUND 可用。
- 两文件锁序：按 ObjId 字典序取两把对象锁（同一文件只取一次），杜绝 A→B 与 B→A 并发拷贝
  互相等待。

### 错误白名单（`errmap.cpp`，RFC 7862 §11.2 五行新增）

SEEK 行含 `NXIO`/`UNION_NOTSUPP`，ALLOCATE/DEALLOCATE 行含 `ROFS/FBIG/NOSPC/DQUOT`，
CLONE/COPY 行另含 `XDEV/LOCKED`，COPY 再含 `OFFLOAD_DENIED`；表外仍降级为 `IO`。
`Nfs4.ErrmapV4Whitelist` 新增对照行。

### backend_local（06 分册映射表 §6.x 三行）

| 方法 | 系统调用 | 备注 |
|------|---------|------|
| `seek` | `lseek(SEEK_DATA/SEEK_HOLE)` | offload 线程；fd 来自 fd 缓存（只读） |
| `allocate` | `fallocate(fd, 0, off, len)` | 写模式 fd；setfsuid 身份模式下切 fsuid |
| `deallocate` | `fallocate(PUNCH_HOLE\|KEEP_SIZE)` | 打洞不改 size（DEALLOCATE 语义） |
| `clone` | `ioctl(FICLONERANGE)` | src_length=0 即到 EOF；ENOTTY 归一为 EOPNOTSUPP |
| `copy_range` | `copy_file_range` 循环 | EXDEV/EOPNOTSUPP/ENOSYS/EINVAL 时退化为 pread/pwrite 循环（同字节语义）；len=0 按源 size 计算 |

**运行时探测**（`LocalBackend::probe_v42_caps`）：启动时在导出目录内开两个 `O_TMPFILE`
（无命名空间痕迹），实测 `fallocate(PUNCH_HOLE)`、`lseek(SEEK_HOLE)`、`FICLONERANGE`，
据此置 `kSparseOps`/`kCopyRange`/`kCloneRange`。`O_TMPFILE` 不可用时回退：稀疏/拷贝按
默认置位（所有主流 fs 支持；拷贝有 pread/pwrite 兜底），CLONE 按 `fstatfs` 魔数（XFS/Btrfs）。
探测结果进启动日志：`export <path> v4.2 capabilities: seek/allocate=… copy=… clone=…`。

跨后端（不同 `[[export]]`）的 `copy_range`/`clone` 在后端边界回 EXDEV——拷贝域 = 一个后端。

### MemoryBackend（测试后端）

字节向量无洞：SEEK_DATA 在 `[0,size)` 内返回原位，SEEK_HOLE 返回 size（EOF 隐式洞），
≥ size 回 ENXIO；DEALLOCATE 写零不缩；ALLOCATE 扩零；CLONE 以拷贝实现。三个能力位全置，
使引擎测试覆盖全部五个 op。

## 验收（开发计划 §8 第 1 项）

- 单测/集成（`lnfs_tests`，115 项）：
  - `Nfs4.MinorversionTwoOpcodeTable`：minor 3 拒绝；同会话 minor 1 下 SEEK→`OP_ILLEGAL`，
    minor 2 下 SEEK 成功、IO_ADVISE→`NOTSUPP`、72→`OP_ILLEGAL`
  - `Nfs4.V42SeekAllocateDeallocate`：SEEK data/hole/eof/NXIO/UNION_NOTSUPP、当前 stateid
    占位符、ALLOCATE 扩展、DEALLOCATE 写零不缩、INVAL 纪律、只读 open→OPENMODE、
    匿名 stateid、目录→ISDIR
  - `Nfs4.V42CopyAndClone`：整文件 COPY（count=0）结果字段、越过源 EOF 的区间 COPY、
    SAVEFH/当前 stateid 占位符单 COMPOUND、跨服 NOTSUPP、溢出 INVAL、OPENMODE、ISDIR、
    缺 SAVEFH→NOFILEHANDLE、CLONE 整文件/区间、他文件 stateid→BAD_STATEID
  - `BackendWrite.V42SparseCopyClone`：真实 fs 上打洞/SEEK/ALLOCATE/copy_range（len=0 与
    区间）字节对照；clone 结果与探测能力位一致；跨后端 EXDEV
  - `Nfs4.ErrmapV4Whitelist` 新增 4.2 行
- 回环验收 `scripts/accept_m6_local.sh`（Release + ASAN 两配置）：新增 `v42` 场景——
  DEALLOCATE/SEEK/ALLOCATE 与后备文件（size、零区、`lseek(SEEK_HOLE)`）逐项对照，整文件
  + 区间 COPY 字节对照，CLONE 依导出 fs 成功或 NOTSUPP，OPENMODE/INVAL/NXIO 纪律，
  minor 1 下 4.2 op `OP_ILLEGAL`；M5 全部阶段（v4rw/v4lock/reclaim/courtesy/ctl/pynfs）
  原样回归
- VM 验收 `scripts/accept_m6_vm.sh`（CI `m6-acceptance`）：`mount -o vers=4.2`；
  `fallocate -p`/`fallocate -l`/`lseek` 洞图与后备文件一致；64 MiB `cp`（内核走 COPY）
  + `copy_file_range` 偏移拷贝字节对照；`cp --reflink=always` 依能力位成功或被拒、
  `--reflink=auto` 回退；cthon basic/general 在 vers=4.2 下回归；v42 场景同服复跑

## 已知边界

- 不做：异步 COPY（copy stateid/CB_OFFLOAD/OFFLOAD_STATUS/CANCEL）、跨服 COPY、
  READ_PLUS（客户端用 READ 即可）、IO_ADVISE、WRITE_SAME、sec_label、xattr（RFC 8276）、
  4.2 新属性（`clone_blksize`/`space_freed`/`change_attr_type`）——客户端逐 op 探测降级。
- CLONE 仅当导出 fs 支持 reflink（XFS reflink=1 / Btrfs）；其他 fs 回 NOTSUPP，Linux
  客户端 `cp --reflink=auto` 自动退到 COPY。
- COPY 结果恒 `UNSTABLE`：客户端随后 COMMIT；验证器与 WRITE 同源（boot epoch）。
- 同一文件内重叠区间的 COPY 交给内核裁决（`copy_file_range` EINVAL → 退化路径逐块拷贝，
  语义为顺序 pread/pwrite）。
- 多网关：与 WRITE 相同的边界（kNativeChange），无新增。

## §8 其余项

| 项 | 处置 | 触发条件 / 理由 |
|----|------|----------------|
| 2. 第二后端（Lustre / GlusterFS） | **未启动** | 需要目标存储的客户端库与测试环境（`liblustreapi` / `libgfapi`），本机均不可得；接口冻结检验需在真实环境下按 06 分册映射表逐项走，出现缺口走 5.10 演进规则 |
| 3. 读委托 + 回传通道发送侧 | **按计划等待真实需求触发** | 计划原文"依赖真实需求触发"；当前无该需求；SessionRec.back 预留位不变 |
| 4. NLM/NSM | **按计划等待 v3 锁刚需** | 计划原文"仅当 v3 锁刚需出现"；v3 与 v4 混用的边界已在部署文档说明 |

三项皆为独立可并行工作，不阻塞本项交付；第 2 项一旦具备环境即为下一优先级。
