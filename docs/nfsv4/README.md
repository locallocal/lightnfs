# NFSv4 协议调研文档

本目录是对 NFS 版本 4 协议族（NFSv4.0 / 4.1 / 4.2）的详细调研，面向 lightnfs 项目的设计与实现。与 [../nfsv3/](../nfsv3/README.md) 的调研文档互相引用对照。

## 规范来源

| 协议 | 规范 | 说明 |
|------|------|------|
| NFSv4.0 | **RFC 7530**（2015，取代 RFC 3530/3010） | 基础版：有状态、COMPOUND、委托、单端口 |
| NFSv4.1 | **RFC 8881**（2020，取代 RFC 5661） | 会话（EOS）、pNFS、目录委托、回传通道 |
| NFSv4.2 | **RFC 7862**（2016） | 服务器端拷贝、稀疏文件、CLONE、安全标签 |
| XDR 定义 | RFC 7531（4.0）、RFC 7863（4.2） | 配套 .x 文件 |
| RPCSEC_GSS | RFC 2203（v1）、RFC 5403（v2）、RFC 7861（v3） | 强制实现的安全机制 |
| ACL/域名映射等勘误与实践 | RFC 8178（小版本规则）、RFC 8587 | 辅助 |

## 文档目录

1. [概述与体系结构](01-overview.md) —— 设计动机、与 v3 的根本差异、三个小版本演进
2. [COMPOUND 与操作总表](02-compound.md) —— 两个过程、COMPOUND 执行模型、全部操作编号
3. [命名空间与属性模型](03-namespace-attrs.md) —— 伪文件系统、bitmap 属性、change 属性、ACL、owner@domain
4. [状态模型](04-state-model.md) —— clientid、stateid、OPEN/CLOSE、锁、租约、宽限期恢复
5. [委托与回调](05-delegations-callbacks.md) —— 读/写委托、CB_COMPOUND、召回流程
6. [NFSv4.1 会话机制](06-sessions-v41.md) —— EXCHANGE_ID/CREATE_SESSION、槽表与 EOS、通道、trunking
7. [pNFS 并行 NFS](07-pnfs.md) —— 布局模型、LAYOUTGET/COMMIT/RETURN、各布局类型
8. [NFSv4.2 新特性](08-v42-features.md) —— COPY/CLONE、SEEK/ALLOCATE、READ_PLUS、安全标签
9. [安全机制](09-security.md) —— RPCSEC_GSS、SECINFO 协商、身份映射、实践中的 AUTH_SYS
10. [错误码](10-errors.md) —— NFS4ERR_* 全表与状态类错误的恢复语义
11. [实现要点与设计建议](11-implementation-notes.md) —— 面向 lightnfs：v3 还是 v4、状态表设计、里程碑

## 快速事实卡

- RPC 程序号仍是 **100003**，版本 **4**；小版本（0/1/2）在 COMPOUND 参数的 `minorversion` 字段中表达，RPC 层不可见
- **单端口 2049/TCP**：不需要 rpcbind、MOUNT、NLM、NSM——全部功能收编进主协议
- 只有 **2 个过程**：NULL(0) 和 **COMPOUND(1)**；真正的操作（v4.2 累计 **59 个**，编号 3–71）作为 COMPOUND 内的操作序列执行
- 回调程序（服务器→客户端）：CB_NULL(0) / CB_COMPOUND(1)，程序号由客户端指定
- **有状态协议**：客户端 ID、打开状态、锁状态、委托、（4.1）会话都由服务器持久跟踪；靠**租约**（lease，典型 60–90 秒）+ 宽限期恢复处理崩溃
- 文件句柄最长 **128 字节**（NFS4_FHSIZE=128，v3 是 64），且允许**易失句柄**（volatile FH）
- 属性不再是定长 fattr3，而是 **bitmap 按需选取**，缓存一致性改用单调的 **change 属性**（不再依赖 mtime）
- 强制 UTF-8 文件名（4.0 严格、后续放宽）；用户/组以 **`user@domain` 字符串**表达（不再是裸 uid/gid）
- 锁内建于协议（LOCK/LOCKT/LOCKU + share reservation），委托（delegation）允许客户端本地缓存并裁决 open/lock
- v4.1 会话提供 **精确一次语义（EOS）**，从协议上取代了 v3 时代"尽力而为"的 DRC
