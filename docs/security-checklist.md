# 安全加固清单落地记录（08 分册 §8.5）

阶段 2 交付 1–4、6 项（开发计划 §4.5）；5（grace reclaim）随阶段 4、7（最小特权/seccomp）与
8（部署文档收尾）随阶段 5 交付。每项列出实现点与验证手段，作为发布前 checklist 的底稿。

## 1. 记录/字段长度上限逐处校验 + fuzz 全入口 ✅

| 层 | 上限 | 实现点 |
|----|------|--------|
| 记录流 | 片段 ≤ `max_fragment`（1MiB）、累计 ≤ `max_request_size`（1MiB+64KiB，可配） | `transport/record_stream.cpp`（违规即断连） |
| RPC 认证体 | ≤ 400B（RFC 5531） | `rpc/rpc_msg.hpp` `kMaxAuthBody` |
| XDR | 所有 `opaque/string` 带显式 max，越界→`kGarbage`→GARBAGE_ARGS | `xdr/xdr.hpp` |
| v3 类型 | 句柄 ≤64B、名字 ≤255、路径 ≤1024、WRITE 数据受记录上限约束 | `nfsv3/nfs3_types.{hpp,cpp}` |
| readdir 预算 | dircount/maxcount 双预算 + TOOSMALL 下限 | `nfsv3/engine.cpp` |

fuzz：`fuzz/fuzz_handle_request.cpp` 直喂 `Dispatcher::handle_request`，阶段 2 起覆盖全部
21 个过程的解码路径（写过程随实现自动纳入）；非 clang 配置以 `fuzz_regress` 回放语料。

## 2. 句柄 HMAC + 每请求导出/IP 校验 ✅

- SipHash-2-4，128-bit 密钥持久化于 `state_dir/hmac.key`（0600，首启生成）——
  `core/file_handle.cpp`。
- 每请求路径：所有过程经 `Engine::resolve → FileHandleCodec::decode`，三分支：
  HMAC 不符→BADHANDLE；fsid 不在导出表→STALE；peer 不在导出 CIDR→ACCES。
- 验证：`tests/test_nfs3.cpp`、`accept_client walk` 负路径（篡改句柄→BADHANDLE）、
  `lightnfs-fh --key` 离线校验工具。

## 3. 名字双层校验 ✅

- 协议层（engine）：`valid_component`（空名、`/`、NUL 拒绝；LOOKUP 允许 `.`/`..`）与
  `valid_new_name`（创建族额外拒 `.`/`..`）——`nfsv3/engine.cpp`。
- 后端层：`LocalBackend::valid_name` 复查 + 所有路径操作 `O_NOFOLLOW`/`*at` 单分量执行，
  导出根 `..` 钳制——`backend/local.cpp`；MemoryBackend 同样复查。
- 验证：`accept_client walk` 空参数写过程→GARBAGE_ARGS；后端契约单测。

## 4. squash 在 auth 层单点完成 ✅

- `ExportTable::squash_cred`（`core/config.cpp`）是唯一映射点：root/all→anon uid/gid，
  组列表清空；引擎每过程在进入后端前调用一次，后端只见映射后的 `Cred`。
- 身份执行（06 §6.4）：模式 1 权限位自查（默认）/`identity="strict"`（faccessat2+fsuid
  复核）/`identity="setfsuid"`（offload 线程切 fsuid，内核权威判定）。

## 6. 资源上限全部有默认值且可配 ✅

| 资源 | 默认 | 配置键 |
|------|------|--------|
| 连接总数 | 4096 | `[server] max_connections` |
| 单 peer 连接 | 128 | `[server] per_peer_limit` |
| 每连接在途 | 64 | `[limits] inflight_per_conn` |
| 请求大小 | 1MiB+64KiB | `[server] max_request_size` |
| DRC 内存/TTL | 64MiB / 120s | `[protocol] drc_mem` / `drc_ttl` |
| fd 缓存 | 4096 | `[export.local] fd_cache` |
| offload 线程 | 16 | `[server] offload_threads` |
| buffer 池 | 分级水位（阶段 0） | 内置，随 per-reactor 池拆分开放配置 |

验证：`accept_client connstorm`（10k 连接 + 超深流水线背压）；超限连接被拒并计数
（`lightnfs_connections_rejected_total`）。

## 5. 宽限期 reclaim 名单强制 ✅（阶段 4）

- 稳定存储仅 `state_dir/{boot_epoch, hmac.key, clients/<hash(co_ownerid)>}`（07 §7.5 红线）；
  CREATE_SESSION 确认后写入名单（fsync），客户端状态全清后延迟删除。
- 重启：epoch++ → 读名单进入 grace（时长 = lease）。grace 内 OPEN/LOCK 的 CLAIM_PREVIOUS/
  reclaim **仅接受名单内客户端**（否则 RECLAIM_BAD），普通建状态操作 → GRACE，匿名写 → GRACE，
  读放行；名单内客户端全部 RECLAIM_COMPLETE → 提前出 grace；RECLAIM_COMPLETE 之后的 reclaim
  → NO_GRACE。
- 实现点：`state/state_mgr.cpp`（`load_grace_list`/`in_grace`/`in_stable_list`/OPEN/lock 门禁）。
- 验证：单测 `StateMgr.GraceReclaimGate`、`Nfs4.RestartReclaimWithinGrace`、`Nfs4.CurrentStateidAndReclaimCompleteGate`；
  `accept_client v4reclaim`（带打开状态 kill -9 重启 → grace 内 reclaim → 数据无损 → 门禁）；
  VM 脚本内核客户端持开文件跨重启写入校验。

## 7. 最小特权运行 + seccomp 白名单 ✅（阶段 5）

- `packaging/systemd/lightnfs.service`：专用系统用户 `lightnfs`；`AmbientCapabilities` =
  `CAP_DAC_READ_SEARCH`（open_by_handle_at 稳定句柄）+ `CAP_NET_BIND_SERVICE`（绑 2049/20048），
  `CapabilityBoundingSet` 同集、其余全 drop、`NoNewPrivileges`。
- 文件系统沙箱：`ProtectSystem=strict` + 仅 `state_dir` 与显式导出树可写；`PrivateTmp`/
  `PrivateDevices`/`ProtectKernel*`/`ProtectProc=invisible`/`MemoryDenyWriteExecute` 等全开。
- seccomp：`SystemCallFilter=@system-service` 去掉 `@clock @debug @module @mount @obsolete
  @raw-io @reboot @swap @cpu-emulation`，再显式加入 `io_uring_{setup,enter,register}` 与
  `name_to_handle_at`/`open_by_handle_at`；越权系统调用返回 EPERM。
- 白名单来源：`scripts/gen_seccomp_allowlist.sh` 对真实 v3+v4.1 读写+锁负载做 strace 取全量
  系统调用集（本机实测集：io_uring_{setup,enter} / name_to_handle_at / open_by_handle_at /
  openat / statx / fsync / linkat / renameat / unlinkat / getdents64 / socket 族 等，
  全部落在上述白名单内）。运行时集变更需重跑并复核单元文件。
- 验证：`systemd-analyze security lightnfs.service` 审计沙箱评分（安装后运行）。

## 8. AUTH_SYS 信任边界写入部署文档 ✅（阶段 5）

- `docs/deployment.md` §1 明确：AUTH_SYS 身份不可验证 ⇒ 仅受信网络部署，公网必须前置
  WireGuard/IPsec 或 TLS；`clients` CIDR 白名单 + `squash` 收敛；句柄 HMAC 防伪造句柄
  但不防伪造身份。§5 归档全部已知限制（无 GSS、无 NLM、v3/v4 混布锁边界、句柄稳定性、
  单机网关）。

---

## 复查记录：错误白名单全覆盖（阶段 5，08 §8.5 隐含 + 开发计划 §7）

- v3：`v3_error_allowed` 逐过程对照 nfsv3 研究分册 08 §8.2（RFC 1813）复查——CREATE 族收敛为
  RFC 行（去掉早期多余的 INVAL/NOTSUPP），MKDIR 增 MLINK（目录链接数上限）、REMOVE/RMDIR 增
  PERM（sticky 目录）为文档化偏差，保留对客户端有意义的 errno。
- v4：`v4_error_allowed` 覆盖全部已实现 op，阶段 5 新增 LOCK/LOCKT/LOCKU（DENIED/BAD_RANGE/
  LOCK_RANGE/LOCK_NOTSUPP/OPENMODE/租约族）、SECINFO/SECINFO_NO_NAME（WRONGSEC/NOENT）、
  FREE_STATEID（LOCKS_HELD）、RECLAIM_COMPLETE、无参 op（GETFH/PUTFH*/SAVEFH/RESTOREFH/
  GETATTR/VERIFY/NVERIFY 只放行通用集）逐条对照 RFC 8881 §15.2。
- 验证：`tests/test_nfs4.cpp::Nfs4.ErrmapV4Whitelist` 扩充 v3/v4 对照断言（越界结果降级为 IO）。
