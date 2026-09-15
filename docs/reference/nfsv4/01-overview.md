# 1. NFSv4 概述与体系结构

## 1.1 为什么重写 NFS

NFSv4 是 IETF 主导（v2/v3 是 Sun 的私有规范拿来发信息性 RFC）的一次彻底重设计，动机直接针对 v3 的痛点：

| v3 的问题 | v4 的回答 |
|-----------|-----------|
| 协议族碎片化（NFS+MOUNT+NLM+NSM+rpcbind，5 个程序、多个端口） | **单协议单端口 2049**，MOUNT/锁/挂载记录全部收编 |
| 防火墙/NAT 不友好（动态端口、服务器反向连客户端做锁回调） | 单端口；4.1 回调复用客户端发起的同一 TCP 连接 |
| 无状态导致锁是外挂（NLM 兼容性差、客户端消失锁滞留） | 有状态 + **租约**：状态有生命周期，超时自动回收 |
| 每操作一次 RPC，广域网延迟放大 | **COMPOUND**：一次往返打包多个操作 |
| 弱缓存一致性，mtime 粒度问题 | 单调 **change 属性** + **委托**（delegation）允许强本地缓存 |
| AUTH_SYS 裸 uid/gid，无安全可言 | 强制实现 RPCSEC_GSS（Kerberos），身份用 `user@domain` |
| 属性定长一刀切，无 ACL | bitmap 按需取属性、Windows 风格 ACE 的 ACL、命名属性 |
| UNIX 中心主义 | 大小写不敏感、hidden/system 属性等跨平台妥协 |
| 重放依赖尽力而为的 DRC | 4.0：owner seqid；4.1：会话槽表，**协议级精确一次语义** |

代价同样明确：**复杂度暴涨**。v3 服务器可以在几千行内做对；v4 的状态机（open/lock/delegation/lease/恢复）、4.1 的会话层，都是数量级更大的工程。

## 1.2 三个小版本

小版本通过 COMPOUND 参数里的 `minorversion` 字段协商，RPC 层永远是 program 100003 / version 4。RFC 8178 规定了小版本演进规则（新小版本只能加操作/属性、不得改变已有语义）。

### NFSv4.0（RFC 7530）

基础形态：COMPOUND、伪文件系统、有状态 OPEN/CLOSE/LOCK、委托、租约恢复、RPCSEC_GSS。遗留缺陷：

- 重放保护靠 per-owner 的 seqid，规则晦涩、易错（OPEN_CONFIRM 之类的补丁操作）；
- 回调需要服务器**反向 TCP 连接**客户端（NAT 后面就废了，委托退化）；
- RENEW 显式续租、DRC 仍然需要。

### NFSv4.1（RFC 8881）

不是小修补，接近半个新协议：

- **会话（session）**：客户端与服务器间建立带**槽表**的会话，每个请求占一个槽 + 序号，服务器按槽缓存应答 ⇒ **精确一次语义（EOS）**，彻底解决重放问题，OPEN_CONFIRM/RENEW 等 4.0 补丁全部废除；
- **回传通道（backchannel）**：回调复用客户端发起的连接，NAT 问题消失，委托真正可用；
- **pNFS**：元数据服务器与数据服务器分离，客户端持"布局"直连数据服务器并行 IO；
- trunking（多连接聚合）、目录委托、RECLAIM_COMPLETE 明确恢复边界。

Linux 客户端默认协商顺序 4.2 → 4.1 → 4.0 → 3；**4.1 是现代部署的事实基线**。

### NFSv4.2（RFC 7862）

在 4.1 会话框架上加特性（全部可选）：服务器端拷贝（COPY/CLONE）、稀疏文件（SEEK/ALLOCATE/DEALLOCATE/READ_PLUS）、IO_ADVISE、WRITE_SAME、安全标签（SELinux）、pNFS 增强（LAYOUTERROR/LAYOUTSTATS）。

## 1.3 协议形态对比（与 v3 并排）

```
NFSv3 部署                          NFSv4 部署
┌────────┬──────┬──────┬──────┐    ┌──────────────────────┐
│ NFS    │MOUNT │ NLM  │ NSM  │    │       NFSv4          │
│ 100003 │100005│100021│100024│    │      100003 v4       │
├────────┴──────┴──────┴──────┤    │  （锁/挂载/回调内建）  │
│    rpcbind :111（发现）      │    ├──────────────────────┤
├──────────────────────────────┤    │   ONC RPC / XDR      │
│    ONC RPC / XDR             │    ├──────────────────────┤
├──────────────────────────────┤    │   TCP :2049（仅此）   │
│    TCP/UDP 多端口            │    └──────────────────────┘
└──────────────────────────────┘    UDP 被明确禁止（4.1 起 RFC 层面）
```

- **不再有 MOUNT 协议**：服务器把所有导出拼成一棵**伪文件系统**（pseudo-fs），客户端 PUTROOTFH 从根开始 LOOKUP 走下去（见 [03-namespace-attrs.md](03-namespace-attrs.md)）。
- **不再有 NLM/NSM**：LOCK/LOCKT/LOCKU 操作 + 租约 + 宽限期内建（见 [04-state-model.md](04-state-model.md)）。
- **传输要求**：必须运行在"可靠有序"传输上，实践即 TCP（4.1 规范性要求；RDMA 亦可，RFC 8267）。

## 1.4 一次典型挂载与访问（对照 v3 时序）

`mount -t nfs4 server:/export /mnt` 后 `cat /mnt/dir/file`（以 4.1 为例）：

```
客户端                                          服务器 :2049
  │ COMPOUND{ NULL探测/EXCHANGE_ID }───────────────▶
  │ ◀── clientid, 服务器能力
  │ COMPOUND{ CREATE_SESSION }─────────────────────▶
  │ ◀── sessionid, 槽表参数（此后每个 COMPOUND 以 SEQUENCE 开头）
  │ COMPOUND{ SEQUENCE; RECLAIM_COMPLETE }
  │ COMPOUND{ SEQUENCE; PUTROOTFH; GETFH; GETATTR }─▶   ← 取伪根
  │ COMPOUND{ SEQUENCE; PUTFH(root); LOOKUP "export";
  │           GETFH; GETATTR(fsid,...) }───────────▶   ← 逐级走到导出点
  │    （挂载完成，无 MOUNT 协议参与）
  │ COMPOUND{ SEQUENCE; PUTFH(dir); OPEN("file", READ);
  │           GETFH; GETATTR }─────────────────────▶   ← OPEN 是有状态的！
  │ ◀── stateid（可能还带读委托）
  │ COMPOUND{ SEQUENCE; PUTFH(file); READ(stateid, 0, 128K) }
  │ COMPOUND{ SEQUENCE; PUTFH(file); CLOSE(stateid) }
```

对照 v3 的关键差异一目了然：

1. 多个操作打包进一个 COMPOUND，往返数大幅减少；
2. 出现了 v3 没有的 **OPEN/CLOSE**——服务器记住"谁打开了什么"；
3. READ/WRITE 必须携带 **stateid**，把 IO 与打开/锁/委托状态关联；
4. 一切从 PUTROOTFH 出发，句柄获取（GETFH）是显式操作。

## 1.5 有状态设计的代价与补偿

v3 的无状态换来了简单的崩溃恢复；v4 引入状态后必须正面回答"状态丢了怎么办"：

- **服务器崩溃**：所有状态丢失。重启后进入**宽限期**（≥ 租约时长）：客户端用 reclaim 型 OPEN/LOCK 重建状态，期间拒绝新申请（NFS4ERR_GRACE）。与 NLM 的宽限期理念相同，但内建且强制。
- **客户端崩溃/消失**：租约到期（客户端不再续租），服务器**自动回收**其全部状态——解决了 NLM"锁永远滞留"的顽疾。
- **网络分区**：租约过期后服务器可把锁给别人；原客户端回来收到 NFS4ERR_EXPIRED，按"锁已丢"处理（应用层感知）。

这套模型贯穿全协议，是 v4 实现复杂度的主要来源，详见 [04-state-model.md](04-state-model.md)。

## 1.6 版本选型速查（lightnfs 视角）

| 需求 | 建议 |
|------|------|
| 受信内网、简单导出、最小实现成本 | 先 NFSv3（见 nfsv3 分册） |
| 穿防火墙/NAT、需要锁语义可靠 | NFSv4.1+ |
| 广域网高延迟 | NFSv4.1+（COMPOUND + 委托 + EOS） |
| 多客户端强共享缓存 | NFSv4.x 委托 |
| 横向扩展数据面 | pNFS（实现代价极高，慎入） |
| 若实现 v4，选哪个小版本 | **直接做 4.1**：4.0 的 seqid/OPEN_CONFIRM/反向回调是历史包袱，4.1 会话反而更规整；4.2 特性全可选，可后补 |
