# lightnfs NFS Gateway 设计文档

lightnfs 是一个用户态 NFS 网关：北向同时提供 **NFSv3 与 NFSv4.1/4.2** 协议服务，南向通过统一的**后端接口（Backend API）**接入多种存储——第一版仅实现本地文件系统（POSIX）后端，但接口按可插拔 Lustre、GlusterFS 等后端的要求一次定义到位。

实现语言/范式：**C++20 + 协程**（全异步、无阻塞事件循环）。

协议语义依据本仓库调研文档：[../nfsv3/](../nfsv3/README.md) 与 [../nfsv4/](../nfsv4/README.md)，本设计不重复协议细节，只引用。

## 文档目录

1. [总体架构](01-architecture.md) —— 目标与非目标、分层、线程与数据流、关键设计决策清单
2. [协程运行时与并发模型](02-runtime-concurrency.md) —— Task/Executor、io_uring reactor、同步原语、取消与超时
3. [传输与 RPC/XDR 层](03-transport-rpc-xdr.md) —— 连接管理、记录标记、RPC 分发、认证、缓冲区与零拷贝
4. [协议核心（v3/v4 双引擎）](04-nfs-core.md) —— 引擎结构、共享语义层、per-object 串行化、WCC/change_info 生成
5. [后端抽象接口（Backend API）](05-backend-api.md) —— **本设计的核心交付物**：类型、能力模型、完整接口定义与语义契约
6. [后端实现](06-backends.md) —— 本地文件系统后端（v1）详设；Lustre/GlusterFS 的映射验证
7. [状态管理](07-state-management.md) —— v3 DRC、v4 clientid/会话/槽表/租约/宽限期、持久化
8. [配置、可观测性与安全](08-config-observability.md) —— 导出表、日志/指标/追踪、资源限制
9. [路线图与里程碑](09-roadmap.md) —— 与 v3/v4 调研分册中 M1–M8 的合并计划

## 一页纸架构

```
                        ┌────────────────────────────────────────────┐
                        │                lightnfsd 进程                │
  mount/rpcbind ─────▶  │  ┌──────────┐  ┌──────────────────────────┐ │
                        │  │ Portmap/ │  │  Transport (TCP :2049)   │ │
  NFSv3 client ──────▶  │  │ MOUNTv3  │  │  conn/records/RPC 解析    │ │
                        │  └────┬─────┘  └───────────┬──────────────┘ │
  NFSv4.x client ────▶  │       │        ┌───────────▼──────────────┐ │
                        │       │        │  RPC Dispatch + Auth     │ │
                        │       │        │  (AUTH_SYS, DRC for v3)  │ │
                        │       │        └─────┬─────────────┬──────┘ │
                        │       │        ┌─────▼─────┐ ┌─────▼──────┐ │
                        │       └───────▶│ V3 Engine │ │ V4 Engine  │ │
                        │                │ (21 procs)│ │ (COMPOUND) │ │
                        │                └─────┬─────┘ └──┬───┬─────┘ │
                        │                      │          │   │ state │
                        │                ┌─────▼──────────▼┐ ┌▼─────┐ │
                        │                │    NFS Core     │ │State │ │
                        │                │ 共享语义/句柄/导出│ │Mgr   │ │
                        │                └────────┬────────┘ └──────┘ │
                        │                ┌────────▼────────┐          │
                        │                │   Backend API   │◀── 本设计核心
                        │                └┬───────┬───────┬┘          │
                        │            ┌────▼───┐┌──▼───┐┌──▼─────┐     │
                        │            │ local  ││lustre││gluster │     │
                        │            │ (v1)   ││(未来)││(未来)  │     │
                        │            └────────┘└──────┘└────────┘     │
                        └────────────────────────────────────────────┘
```

## 关键设计决策（详见各分册）

| # | 决策 | 选择 | 分册 |
|---|------|------|------|
| D1 | 协程运行时 | 自研薄运行时（`Task<T>` + io_uring reactor，epoll 兜底），不绑定 asio/seastar | 02 |
| D2 | 线程模型 | N 个 reactor 分片，连接绑定分片；文件 IO 走 io_uring，不可 uring 化的后端调用走 offload 线程池 | 02 |
| D3 | v3/v4 共存方式 | 双引擎 + 共享 NFS Core；**文件句柄两版通用（≤64B）** | 01/04 |
| D4 | 后端接口形态 | 句柄式（handle-based，仿 Ganesha FSAL 教训改良）：`ObjId` 持久标识 + `Object` 活对象 + 能力位协商 | 05 |
| D5 | v4 小版本 | 仅 4.1/4.2，不实现 4.0（调研分册 nfsv4/11 的结论） | 04/07 |
| D6 | 错误模型 | `Result<T> = expected<T, Errno>`，errno 在协议引擎边界统一映射为 nfsstat3/nfsstat4 | 04 |
| D7 | 锁 | 网关内统一锁表（后端锁能力作为可选下沉接口，v1 不下沉） | 05/07 |
| D8 | 委托/pNFS | v1 不发委托、不支持 pNFS；接口预留 | 04/09 |
