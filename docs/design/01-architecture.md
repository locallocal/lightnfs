# 1. 总体架构

## 1.1 目标与非目标

**目标**

1. 单进程用户态 NFS 网关 `lightnfsd`，北向同时服务 NFSv3（含 MOUNTv3、可选 portmap 内嵌）与 NFSv4.1/4.2，单端口 2049 + MOUNT 辅助端口。
2. 南向后端可插拔：v1 交付本地文件系统（POSIX）后端；接口设计必须经受 Lustre（POSIX 挂载 + liblustreapi 优化）与 GlusterFS（libgfapi，无本地挂载）两个假想后端的映射验证（见 [06-backends.md](06-backends.md)）。
3. C++20 协程全异步：任何网络/磁盘等待都不阻塞 reactor 线程；单机万级并发连接、百万级缓存对象可运行。
4. 协议正确性优先：v3/v4 调研分册中标注"红线"的语义（verifier、WCC 原子性、句柄持久性、宽限期名单等）全部落实。

**非目标（v1 明确不做，但接口不堵死）**

- pNFS（任何角色）、写/目录委托、v4.0（minorversion=0）、NLM/NSM（v3 锁）、RPCSEC_GSS/krb5、UDP 传输、HA/多活。
  （**读委托 + 回传通道**见 M8/plan doc 10 §5.2 已交付；**RPC-over-TLS**（RFC 9289）见 09 册
  长期观察项、plan doc 10 §5.4 已落地——传输层加密，身份仍 AUTH_SYS。）
- 网关内数据缓存（页缓存归 OS/后端管；网关只做元数据/句柄/readdir 游标等小缓存）。

## 1.2 分层与模块

自上而下（对照 README 架构图）：

| 层 | 模块 | 职责 | 关键约束 |
|----|------|------|----------|
| L1 传输 | `transport` | listen/accept、TCP 记录标记帧、连接生命周期、背压 | 不理解 RPC 语义 |
| L2 RPC | `rpc` | RPC 头解析、xid、认证（AUTH_SYS/NONE）、程序分发、应答编排 | 不理解 NFS 语义 |
| L3 引擎 | `v3engine` / `v4engine` / `mountd` | 过程/COMPOUND 解释、XDR 参数结果编解码、协议状态机 | 只与 L4 交互，不直接碰后端 |
| L4 核心 | `core` | 导出表、文件句柄编解码、权限/squash、per-object 串行化、WCC/change_info 采样、DRC(v3)、StateMgr(v4) | 协议无关的"NFS 语义中枢" |
| L5 后端 | `backend` API + `backend_local` | 存储操作的统一异步接口 | 协议完全无感 |
| 横切 | `runtime` / `xdr` / `util` / `obs` | 协程运行时、XDR 基建、日志指标 | — |

依赖方向严格单向：L1→L2→L3→L4→L5。L3 的两个引擎互不依赖，全部共享语义放 L4——这是"v3/v4 行为一致"的结构保证（例如同一文件的 change/WCC 采样逻辑只有一份）。

## 1.3 线程与数据流

```
                 ┌─ reactor #0 (io_uring) ── conn a, conn b, …
   accept 线程 ──┼─ reactor #1 ───────────── conn c, …
   (轮转指派)    └─ reactor #N-1
                        │  co_await 后端操作
                        ├───────────────► io_uring 文件 IO（read/write/fsync/statx…）
                        └───────────────► offload 池（openat/rename 等无 uring 支持的调用、
                                                     未来 libgfapi 等阻塞库）
```

- **连接绑定 reactor**：一条连接的全部请求在同一 reactor 上解析/应答，连接内无跨线程同步；请求处理协程默认也在本 reactor 恢复。
- **跨连接共享状态**（v4 状态表、DRC、句柄缓存、per-object 锁）按 key 分片 + 异步互斥（见 02 分册），避免全局锁。
- 一次 v3 READ 的完整数据流（v4 同构，多一层 COMPOUND 循环）：

```
[reactor] readv 拼出完整记录 → rpc 解析 → v3engine.READ 解码
   → core: 句柄→ObjId、导出/权限检查
   → backend_local.read(obj, off, len, buf)      ← co_await io_uring pread
   → 应答直接编码进发送缓冲（数据零拷贝段）→ writev
```

## 1.4 进程与部署形态

- 单进程多线程；`--reactors N`（默认=物理核数）、`--offload M`。
- 启动序：加载配置 → 初始化后端（每导出一个 Backend 实例）→ 恢复持久状态（boot epoch、v4 客户端名单）→ 监听 2049/MOUNT 端口 → 注册 rpcbind（可选，`--no-rpcbind` 供纯 v4 部署）。
- 平滑退出：停 accept → 等在途请求（有界超时）→ v4 状态落盘（尽力）→ 关闭后端。
- 崩溃恢复语义完全依赖协议机制（verifier 变化、宽限期），不依赖退出时落盘成功。

## 1.5 请求生命周期与背压

1. 记录标记长度预检（上限 = `max_request_size`，默认 1MiB + 64KiB 头部余量）→ 超限断连。
2. 每连接在途请求上限（v3：配置值，默认 64；v4.1：会话槽数天然限定）；达到上限即停读该连接 socket（TCP 背压回推客户端）。
3. 每请求一个协程：`handle_request()` 从解析到应答发送排队为一个 co_await 链；异常在引擎边界转为 SERVERFAULT 类错误，绝不逃逸到 reactor。
4. 应答按完成顺序发送（xid 乱序合法，v3/v4 皆然）；同一连接的发送由 per-conn 发送队列串行化。

## 1.6 一次定型的公共资产（v3/v4 共用）

- **文件句柄格式**（≤64B，两版通用，见 04 分册 4.3）：v3 客户端与 v4 客户端看到同一对象的同一句柄，跨版本重挂载缓存不失效。
- **write verifier / boot epoch**：全局一个，v3 WRITE/COMMIT 与 v4 共用。
- **导出表**：v3 MOUNT 检查、v4 伪文件系统、NFS 层每请求校验，同一份数据三处消费。
- **权限/身份**：AUTH_SYS 解析 + squash 映射 → `Cred{uid,gid,gids[]}`，两引擎入口统一生成，向下传递到后端。

## 1.7 源码布局（规划）

```
src/
  runtime/    task.hpp scheduler.cpp uring_reactor.cpp offload_pool.cpp sync.hpp
  xdr/        xdr.hpp（编解码原语） nfs3_types.hpp nfs4_types.hpp（生成或手写）
  transport/  listener.cpp connection.cpp record_stream.cpp
  rpc/        rpc_msg.hpp auth.cpp dispatch.cpp drc.cpp
  engine3/    v3_engine.cpp mountd.cpp portmap.cpp
  engine4/    v4_engine.cpp compound.cpp ops/*.cpp session.cpp
  core/       exports.cpp fh.cpp cred.cpp objlock.cpp nfscore.cpp
  state/      clientdb.cpp state_table.cpp lease.cpp grace.cpp lockmgr.cpp
  backend/    api.hpp（05 分册的接口） registry.cpp
  backend_local/ local_backend.cpp fd_cache.cpp dircursor.cpp
  obs/        log.hpp metrics.cpp
  main.cpp config.cpp
```
