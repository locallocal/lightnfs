# M7：v4 锁与安全完备（阶段 5）

在阶段 4 读写+状态全量之上补齐 NFSv4.1 字节区间锁与发布前安全加固。此阶段结束即 **v1
发布候选**。v3 路径与后端接口不变。

## 实现要点

### 网关内 LockMgr（设计 07 §7.6 / 05 §5.8）

`src/state/lock_mgr.{hpp,cpp}`——per-ObjId 有序区间段表，实现 `backend::LockMgr` 接口（v1
唯一实现者；后端 `native_locks()` 有值时状态层改用后端实现，切换点唯一）：

- **POSIX 语义**：同 owner 在其范围内的既有段被替换（升级/降级/拆分），相邻同类型段合并
  （coalesce）；length=UINT64_MAX 表"到 EOF"。
- **非阻塞裁决**（RFC 8881 §18.10）：另一 owner 的重叠且至少一方排他 → 返回第一个冲突段
  （owner + 区间 + 类型），不排队；W 后缀（READW/WRITEW）"愿意等待"仍立即 DENIED（v1 不发
  CB_NOTIFY_LOCK）。
- 纯表操作，16 分片 `std::mutex`，无 IO、无协程挂起；统计 `total_segments`/`files_locked`。

### 锁状态层（07 §7.1 扩展）

`state_mgr.cpp`——`StateRec` 增 `kLock` 类型（`lowner` = {clientid|fnv64(owner)|len}、
`parent_open` 关联派生的 open）；`FileStateRec.locks` 每 (lock-owner, file) 一个 lock stateid：

- **LOCK**：new_owner 从 open stateid 派生（校验 open 存在/属主/access 覆盖锁类型 → 否则
  OPENMODE），existing 从既有 lock stateid；授予则复用/新建 lock stateid 并 seqid++；冲突
  DENIED 带持有者 clientid+owner；courtesy 持有者的冲突先回收再重裁（07 §7.4）。
- **LOCKT**：只测不建，自身锁不冲突。**LOCKU**：精确区间解锁 + seqid++。
- **stateid 纪律**：0=当前、旧 OLD_STATEID、超前 BAD_STATEID；grace 门禁与 OPEN 一致
  （reclaim 仅名单内、否则 GRACE/NO_GRACE/RECLAIM_BAD）。
- **生命周期**：IO 经 lock stateid 时按 parent open 的 access 判 OPENMODE；CLOSE 连带释放该
  open 派生的全部 lock 状态与区间（§18.2.4）；FREE_STATEID 仅当区间已空（否则 LOCKS_HELD）；
  租约回收/客户端重启走同一条回收链；`unlink_state` 对 lock 类型只减 lock_count（修正了
  open_count 误减）。
- 指标：`lock_states`/`lock_segments`/`lock_denied`；`lightnfs-ctl state` dump 增 `lock` 行
  （含区间与 parent）。

### SECINFO / SECINFO_NO_NAME（RFC 8881 §18.29/§18.45）

AUTH_SYS-only 服务器：名字解析成功后恒返回 `[AUTH_SYS]`，永不发 WRONGSEC（合规简化）；
消费当前句柄（§2.6.3.1.1.8，后续 GETFH → NOFILEHANDLE）；名字经空/UTF-8/BADNAME 校验。
SECINFO_NO_NAME（阶段 3 已有）保持。

### 错误白名单全覆盖复查（开发计划 §7 / 08 §8.5）

`src/core/errmap.cpp` 逐 op/过程对照调研分册：

- **v3**（nfsv3 08 §8.2）：CREATE 族收敛为 RFC 1813 行；MKDIR 增 MLINK、REMOVE/RMDIR 增 PERM
  为文档化偏差（保留对客户端有意义的 errno）。
- **v4**（RFC 8881 §15.2）：新增 LOCK/LOCKT/LOCKU、SECINFO、SECINFO_NO_NAME、FREE_STATEID、
  RECLAIM_COMPLETE、无参句柄 op 的允许集；越界结果降级 IO/SERVERFAULT。
- 单测 `Nfs4.ErrmapV4Whitelist` 扩充 v3/v4 对照断言。

补记（2026-08-23，阶段 6 §9 基建）：v3 白名单改为由调研分册 §8.2 表生成式对照
（`scripts/gen_errmap_cases.py`），首轮即发现 LINK 行遗漏 NOSPC（RFC 1813 §3.3.15），已补。

### 安全加固收尾（08 §8.5 第 5/7/8 项）

- **最小特权**：`packaging/systemd/lightnfs.service`——专用用户 + 仅
  `CAP_DAC_READ_SEARCH`+`CAP_NET_BIND_SERVICE`、`CapabilityBoundingSet` 同集、
  `NoNewPrivileges`；`ProtectSystem=strict` + 仅 state_dir/导出树可写 + 全套 `Protect*`；
  seccomp `@system-service` 去高危集 + 显式加 `io_uring_*`/句柄系统调用，越权 EPERM。
  白名单由 `scripts/gen_seccomp_allowlist.sh` 从真实 v3+v4.1 读写+锁负载 strace 生成。
- **部署文档**：`docs/deployment.md`——AUTH_SYS 信任边界（仅受信网络、公网前置
  WireGuard/IPsec/TLS）、最小特权安装、配置要点、ctl/Prometheus 运维、已知限制归档。
- 安全清单 `docs/security-checklist.md` 第 5/7/8 项 + 错误白名单复查记录归档。

## 验收（开发计划 §7）

**回环半（无 root）**：`scripts/accept_m5_local.sh`——两配置单测（含新增
`LockMgr.*`、`StateMgr.ByteRangeLocksLifecycle`、`Nfs4.LockOpsAndSecinfo`）；
`accept_client v4lock`（双客户端 LOCK/LOCKT/LOCKU、DENIED 带持有者、new/existing owner、
升级+LOCKU 拆分、CLOSE 释放锁、数据无损）；v4rw/reclaim/courtesy/ctl 回归；pynfs 4.1
currentstateid + courteous（均驱动 LOCK）+ secinfo_no_name + SEC1/SEC2（带名 SECINFO）+
阶段 3/4 组；ASAN 泄漏检查。**本机实测：pynfs 锁/secinfo/courtesy 组 26 通过 / 1 失败
（CSID7 为 pynfs 自身 NameError）。**

**真实 mount 半（root VM，CI `m5-acceptance`）**：`scripts/accept_m5_vm.sh`——
`mount -o vers=4.1`：cthon04 basic/general/special/**lock** 组、内核 POSIX 字节锁（fcntl
F_SETLK/F_GETLK 冲突与释放）、fsx、带打开状态 kill -9 重启 reclaim、租约回收两路径、
v3/v4 混写。

## 已知边界

- W 后缀锁不发 CB_NOTIFY_LOCK（v1 无回传通道，客户端轮询）；无死锁检测（非阻塞锁无死锁）。
- v3 无锁语义：v3 写不受 v4 字节锁/share deny 约束（同后端混布，文档明示）。
- SECINFO 恒 `[AUTH_SYS]`；无 krb5/RPCSEC_GSS（设计取舍 D8）。
- 无特权回退句柄在无 STATX_BTIME 的文件系统（tmpfs）上不跨重启稳定（生产用内核句柄）。
