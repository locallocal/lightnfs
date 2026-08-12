# NFSv3 协议调研文档

本目录是对 NFS 版本 3 协议（NFSv3）及其配套协议的详细调研，面向 lightnfs 项目的设计与实现。

## 规范来源

| 协议 | 规范 | 说明 |
|------|------|------|
| NFSv3 | RFC 1813 (1995) | NFS Version 3 Protocol Specification（信息性 RFC，事实标准） |
| ONC RPC | RFC 5531（前身 RFC 1831/1057） | 远程过程调用框架，NFS 的传输承载层 |
| XDR | RFC 4506（前身 RFC 1832/1014） | 外部数据表示，RPC 消息的序列化格式 |
| rpcbind / portmap | RFC 1833 | RPC 程序号 → 端口号的注册与查询服务 |
| MOUNT v3 | RFC 1813 附录 I | 获取导出目录初始文件句柄 |
| NLM v4 | RFC 1813 附录 II（结合 X/Open XNFS） | 网络锁管理器（文件锁） |
| NSM v1 | X/Open XNFS | 网络状态监控（崩溃恢复通知） |

## 文档目录

1. [协议概述与体系结构](01-overview.md) —— NFSv3 的定位、设计目标、与 v2/v4 的对比、协议栈全景
2. [ONC RPC 与 XDR 基础](02-rpc-xdr.md) —— RPC 消息格式、认证、传输层、XDR 编码规则
3. [基本数据类型与结构](03-data-types.md) —— 文件句柄、fattr3、sattr3、WCC、常量定义
4. [NFSv3 的 21 个过程详解](04-procedures.md) —— 每个过程的参数、结果、语义与实现注意点
5. [MOUNT 协议](05-mount-protocol.md) —— 挂载流程、导出管理、安全模型
6. [NLM 与 NSM（文件锁）](06-nlm-nsm.md) —— 锁协议、宽限期、崩溃恢复
7. [缓存与一致性模型](07-caching-consistency.md) —— close-to-open、WCC、属性缓存、写回与 COMMIT
8. [错误码大全](08-errors.md) —— 全部错误码及各过程允许返回的错误
9. [实现要点与设计建议](09-implementation-notes.md) —— 文件句柄设计、DRC、幂等性、性能与安全，面向 lightnfs

## 快速事实卡

- RPC 程序号：NFS = **100003**（版本 3），MOUNT = **100005**（版本 3），NLM = **100021**（版本 4），NSM = **100024**（版本 1），rpcbind = **100000**（端口 111）
- NFS 服务器惯用端口：**2049**（TCP/UDP，NFSv3 本身不强制，靠 rpcbind 发现）
- 过程数：NFSv3 共 **22 个过程**（编号 0–21，含 NULL）
- 文件句柄：**不透明字节串，最长 64 字节**，可变长度
- 文件大小 / 偏移：**64 位**（v2 为 32 位）
- 最大读写块：由服务器通过 FSINFO 通告（rtmax/wtmax），协议本身不设上限
