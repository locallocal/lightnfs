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
