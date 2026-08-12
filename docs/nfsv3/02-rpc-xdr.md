# 2. ONC RPC 与 XDR 基础

NFSv3 的每一次交互都是一次 ONC RPC 调用。要实现 NFS 服务器，必须先实现（或引入）RPC 消息编解码与 XDR 序列化。本文覆盖实现所需的全部细节。

## 2.1 XDR（RFC 4506）

XDR（External Data Representation）是一种**大端、4 字节对齐**的二进制序列化格式。没有字段标签、没有自描述信息——收发双方必须共享同一份接口定义（`.x` 文件）。

### 基本类型编码规则

| 类型 | 编码 |
|------|------|
| `int` / `unsigned int` | 4 字节大端 |
| `hyper` / `unsigned hyper` | 8 字节大端（NFSv3 的 size/offset 用它） |
| `bool` | 4 字节，0 = FALSE，1 = TRUE |
| `enum` | 同 int |
| `float` / `double` | IEEE 754，4 / 8 字节（NFS 不用） |
| 定长 opaque `opaque x[n]` | n 字节数据 + 填充到 4 的倍数（填 0） |
| 变长 opaque `opaque x<m>` | 4 字节长度 + 数据 + 填充到 4 的倍数 |
| `string s<m>` | 同变长 opaque（无 NUL 终止符，长度显式给出） |
| 定长数组 `T x[n]` | n 个元素依次编码，无长度前缀 |
| 变长数组 `T x<m>` | 4 字节元素个数 + 各元素依次编码 |
| `struct` | 各字段按声明顺序依次编码，无对齐间隙（天然 4 字节对齐） |
| `union switch (E d)` | 4 字节判别值 + 对应分支的编码 |
| `T *p`（optional） | 等价于 `T p<1>`：4 字节 0/1 + （若 1）一个 T。链表就是靠它编码的 |
| `void` | 0 字节 |

实现提示：

- **所有东西都是 4 字节的倍数**。解码器只需一个游标；遇到变长字段先读长度、做上限校验（防御恶意长度导致的内存放大攻击）、再取整到 4 的倍数前进。
- NFSv3 中的链表（如 READDIR 的目录项）编码为：`bool 有下一项` + 项内容，重复直到 `FALSE`。流式生成/解析都很方便。

### NFSv3 使用的 typedef

```c
typedef unsigned hyper uint64;
typedef hyper          int64;
typedef unsigned int   uint32;
typedef int            int32;
typedef string         filename3<>;    /* 目录项名，单分量 */
typedef string         nfspath3<>;     /* 符号链接内容 */
typedef uint64         fileid3;        /* inode 号 */
typedef uint64         cookie3;        /* 目录遍历游标 */
typedef opaque         cookieverf3[NFS3_COOKIEVERFSIZE];  /* 8 字节 */
typedef opaque         createverf3[NFS3_CREATEVERFSIZE];  /* 8 字节 */
typedef opaque         writeverf3[NFS3_WRITEVERFSIZE];    /* 8 字节 */
typedef uint32         uid3;
typedef uint32         gid3;
typedef uint64         size3;
typedef uint64         offset3;
typedef uint32         mode3;
typedef uint32         count3;
```

## 2.2 ONC RPC 消息格式（RFC 5531）

### 调用消息（CALL）

```
uint32  xid           事务 ID，客户端生成，应答原样带回；重传沿用同一 xid
uint32  msg_type      0 = CALL
uint32  rpcvers       必须为 2（RPC 协议版本，不是 NFS 版本）
uint32  prog          程序号：NFS=100003, MOUNT=100005, NLM=100021...
uint32  vers          程序版本：3（NFS3）
uint32  proc          过程号：0..21（NFS3）
opaque_auth cred      凭证
opaque_auth verf      验证子（AUTH_SYS 下为 AUTH_NONE 空值）
...                   过程参数（XDR 编码）
```

`opaque_auth` 结构：

```
uint32  flavor        0=AUTH_NONE, 1=AUTH_SYS, 2=AUTH_SHORT, 6=RPCSEC_GSS
opaque  body<400>     具体内容，最长 400 字节
```

### 应答消息（REPLY）

```
uint32  xid
uint32  msg_type      1 = REPLY
uint32  reply_stat    0 = MSG_ACCEPTED, 1 = MSG_DENIED
```

MSG_ACCEPTED 分支：

```
opaque_auth verf      服务器验证子（AUTH_SYS 下用 AUTH_NONE 空值）
uint32  accept_stat:
        0 SUCCESS        后接过程结果
        1 PROG_UNAVAIL   程序号未注册
        2 PROG_MISMATCH  版本不支持（后接支持的 low/high 版本）
        3 PROC_UNAVAIL   过程号不存在
        4 GARBAGE_ARGS   参数解码失败
        5 SYSTEM_ERR     服务器内部错误
```

MSG_DENIED 分支：

```
uint32  reject_stat:
        0 RPC_MISMATCH   RPC 版本不是 2（后接 low/high）
        1 AUTH_ERROR     认证失败（后接 auth_stat 原因码）
```

**关键区分**：RPC 层错误（如 GARBAGE_ARGS）与 NFS 层错误（如 NFS3ERR_NOENT）是两个层面。NFS 层错误在 accept_stat=SUCCESS 的结果体内以 `nfsstat3` 表达。参数格式非法用 GARBAGE_ARGS；参数格式合法但语义无效（如句柄字段超 64 字节）应在 NFS 层返回 NFS3ERR_BADHANDLE / NFS3ERR_INVAL。

### AUTH_SYS 凭证体

```
uint32  stamp         任意值（通常是时间戳）
string  machinename<255>
uint32  uid
uint32  gid
uint32  gids<16>      附属组，最多 16 个
```

服务器直接信任这些 uid/gid——这就是 NFSv3 在非受信网络中不安全的根源。实现时注意：

- 附属组超过 16 个会被截断（客户端内核负责截断），服务器端可选做组扩展（`--manage-gids` 之类）。
- root squash：若配置了 squash，服务器应在授权前把 uid 0 映射为匿名 uid（惯例 65534）。

### RPCSEC_GSS（RFC 2203）概要

flavor=6，基于 GSS-API（实践中即 Kerberos 5）。三个服务级别：krb5（仅认证）、krb5i（完整性）、krb5p（加密）。有独立的上下文建立子协议（RPCSEC_GSS_INIT 等控制过程），数据交换阶段对参数/结果做 MIC 或 wrap。轻量实现通常先只做 AUTH_NONE + AUTH_SYS，把 GSS 留作扩展点。

## 2.3 传输层

### TCP：记录标记（Record Marking）

TCP 是字节流，RPC 消息边界靠**记录标记**划分。每个记录片段前有 4 字节头：

```
bit 31        : 最后一个片段标志（last fragment）
bits 30..0    : 本片段长度（字节）
```

一条 RPC 消息 = 1 到多个片段，最后一个片段置最高位。实践中几乎所有实现每条消息只发一个片段。**解析时必须处理多片段**，并对总长度设上限（防 DoS；典型上限 = 最大写请求 + 头部余量，比如 1MB wtmax 时限 ~1MB+16KB）。

TCP 下同一连接上请求可以**流水线并发**，应答可以乱序返回（靠 xid 匹配）。服务器应支持读取多个未完成请求并发处理。

### UDP

一个 UDP 数据报承载一条完整 RPC 消息，无记录标记。受数据报大小限制（惯例上 NFS/UDP 读写块 ≤ 32KB，且依赖 IP 分片，丢包代价大）。现代部署几乎都用 TCP；lightnfs 可以只实现 TCP，但 rpcbind 注册时注意只注册 tcp netid。

### 重传与 DRC 的关系

- UDP：客户端超时重传（同一 xid）；服务器无 DRC 时非幂等操作会被重复执行。
- TCP：正常不重传，但**连接断开重连后**客户端会重发未应答请求，问题同样存在。
- 所以 DRC 对 TCP 同样必要（详见 [09-implementation-notes.md](09-implementation-notes.md)）。

## 2.4 rpcbind / portmap（RFC 1833）

RPC 服务启动时向本机 rpcbind（端口 111）注册 (prog, vers, netid, addr)；客户端先查询 rpcbind 拿端口再连接目标服务。

- **portmap v2**（老协议）：过程 GETPORT(prog, vers, prot, port) → 端口号；SET/UNSET 注册注销。prot 取值 6=TCP、17=UDP。
- **rpcbind v3/v4**：地址用 universal address 字符串（如 `192.168.1.5.8.1` 表示端口 2049 = 8*256+1）。
- Linux 客户端 mount 时通常直连 2049 端口探测 NFS，再查 rpcbind 找 MOUNT。lightnfs 需要注册 NFS(100003,3) 和 MOUNT(100005,3)；也可以选择自带一个极简 portmap 实现或依赖系统 rpcbind。

## 2.5 rpcgen 与 .x 接口文件

RFC 1813 全文附带了完整的 XDR 接口定义（`nfs3.x`），可以用 `rpcgen` 生成 C 的编解码桩代码。自研实现时，即使不用 rpcgen，也强烈建议把 RFC 中的 .x 定义作为编解码器的单一事实来源，逐个结构手写或代码生成 encode/decode 函数并配对做 round-trip 测试。
