# 6. NLM 与 NSM：NFSv3 的文件锁体系

NFSv3 本体无状态，因而**协议本身不含文件锁**。POSIX 咨询锁（fcntl/F_SETLK）由两个辅助协议提供：

- **NLM（Network Lock Manager）** v4：程序号 **100021**，版本 4（v4 配 NFSv3，因为要支持 64 位偏移；v1–3 配 NFSv2）。
- **NSM（Network Status Monitor）** v1：程序号 **100024**。锁是状态，状态就要处理崩溃——NSM 负责崩溃重启的互相通知。

规范来源：RFC 1813 附录 II 只给了 NLM v4 的 XDR 变化；完整语义在 X/Open XNFS 规范（C702）。这是 NFSv3 生态中最含糊、实现兼容性问题最多的部分。

## 6.1 模型概述

- 锁是**咨询锁（advisory）**：只约束同样来申请锁的进程，不阻止无锁读写。
- 锁的持有者标识：(调用方主机名 caller_name, 进程标识 svid/oh)。`oh`（owner handle）是不透明字节串。
- 锁定对象用 **NFS 文件句柄** 指认（NLM 与 NFS 服务器必须共享句柄空间——两者必须是同一台服务器上的配套服务）。
- 区间锁：offset + length（uint64），length=0 表示"到文件末尾"；支持共享锁（读锁）/排它锁（写锁）。

## 6.2 NLM v4 过程

同步接口（客户端阻塞等应答）：

| # | 过程 | 说明 |
|---|------|------|
| 0 | NULL | 探活 |
| 1 | TEST | 测试锁是否可获得（F_GETLK）；冲突时返回持有者信息 |
| 2 | LOCK | 加锁；`block=TRUE` 时若冲突返回 LCK_BLOCKED，稍后服务器回调 GRANTED |
| 3 | CANCEL | 取消一个正在排队的阻塞锁请求 |
| 4 | UNLOCK | 解锁 |
| 5 | GRANTED | **服务器→客户端回调**：先前阻塞的锁现在给你了 |

异步消息接口（6–15：TEST_MSG/LOCK_MSG/.../GRANTED_RES）：语义与同步版相同，只是拆成"请求消息 + 对方回发结果消息"两次单向 RPC。历史遗留，现代客户端（Linux）用同步接口；**服务器可以只实现同步接口 + GRANTED 回调**。

DOS 风格共享锁（20 SHARE / 21 UNSHARE，配 deny 模式）与 22 NM_LOCK（非监控锁，给没有 NSM 的客户端用）、23 FREE_ALL：按需实现，Linux 客户端正常路径不用 SHARE/UNSHARE。

### 关键结构

```c
struct nlm4_lock {
    string   caller_name<MAXNETOBJ_SZ>;
    netobj   fh;         /* NFS 文件句柄 */
    netobj   oh;         /* 锁属主不透明标识 */
    int32    svid;       /* 进程号 */
    uint64   l_offset, l_len;
};

enum nlm4_stats {
    NLM4_GRANTED = 0, NLM4_DENIED = 1, NLM4_DENIED_NOLOCKS = 2,
    NLM4_BLOCKED = 3, NLM4_DENIED_GRACE_PERIOD = 4, NLM4_DEADLCK = 5,
    NLM4_ROFS = 6, NLM4_STALE_FH = 7, NLM4_FBIG = 8, NLM4_FAILED = 9
};
```

LOCK 参数中还有 `bool block`（愿意排队等）、`bool exclusive`、`int32 state`（NSM 状态计数）、`bool reclaim`（宽限期内重收旧锁）。

## 6.3 阻塞锁流程

```
客户端                         服务器
  │ LOCK(block=TRUE) ──────────▶ 冲突
  │ ◀────────── NLM4_BLOCKED     （请求进等待队列）
  │         ……持锁者 UNLOCK……
  │ ◀────────── GRANTED 回调（服务器作为 RPC 客户端反向调用）
  │ GRANTED 应答(NLM4_GRANTED) ─▶
```

- GRANTED 回调需要服务器**主动向客户端的 NLM 服务发起 RPC**（查客户端 rpcbind）。回调丢失怎么办：客户端会超时轮询重发 LOCK；服务器也可重发 GRANTED。
- 简化实现路线：对 block=TRUE 也直接返回 NLM4_BLOCKED 或 DENIED，让客户端轮询——合法但体验差；或干脆宣布不支持锁（见 6.6）。

## 6.4 NSM（statd）与崩溃恢复

NSM 过程：0 NULL, 1 STAT, 2 **MON**（请求监控某主机）, 3 UNMON, 4 UNMON_ALL, 5 SIMU_CRASH, 6 **NOTIFY**（"我重启了"通知）。

核心机制——**状态计数（state number）**：每台主机的 NSM 维护一个单调递增计数，**每次重启加 1 变成奇数**（运行中为奇数，历史约定不必纠结奇偶，关键是"变了=重启过"）。

崩溃恢复两个方向：

1. **服务器崩溃重启**：服务器 NSM 向所有被监控的客户端发 NOTIFY(新 state)。客户端 lockd 收到后进入**重收锁（reclaim）**流程：在服务器**宽限期（grace period，典型 45–90 秒）**内用 `reclaim=TRUE` 的 LOCK 重新申请崩溃前持有的所有锁。宽限期内服务器**只接受 reclaim 锁**，普通新锁一律回 NLM4_DENIED_GRACE_PERIOD。
2. **客户端崩溃重启**：客户端 NSM 重启后 NOTIFY 服务器；服务器**丢弃该主机持有的全部锁**（否则死锁永存——无租约超时机制，这是 NLM 最大的设计弱点：客户端断电不重启，锁就永远留着，只能人工清理）。

MON：lockd 第一次与某对端建立锁关系时，向**本机** statd 注册"帮我盯着对方主机"；statd 间通过 NOTIFY 互通。

## 6.5 与本地锁的联动

NLM 服务器应把网络锁体现到底层文件系统（如 Linux 上通过 lockd 与 VFS 锁表集成），使本地进程与 NFS 客户端互相可见。用户态 NFS 服务器（如 Ganesha、unfsd）通常只在自己进程内维护锁表——远端之间互斥正确，但与服务器本地进程不互斥，需在文档中注明。

## 6.6 lightnfs 的现实选项

按投入从小到大：

1. **不支持锁**：不注册 100021/100024。客户端 `flock/fcntl` 会挂起或报 ENOLCK；挂载加 `-o nolock` 可正常使用（本地模拟锁）。对很多只读/单写场景足够，文档写清楚即可。
2. **进程内锁表 + 同步接口**：实现 NULL/TEST/LOCK/UNLOCK/CANCEL + GRANTED 回调，锁表放内存；NSM 只实现到"服务器重启后换 verifier/state，宽限期内拒绝新锁"的最小闭环，不做对客户端的主动监控（接受"客户端消失锁滞留"，配管理命令清锁）。
3. **完整 NLM+NSM**：含 MON/NOTIFY、reclaim、异步消息接口。工作量大，兼容性测试繁重，除非明确需要多客户端写共享+锁，否则不建议先做。

建议 lightnfs 从选项 1 起步，接口上为选项 2 预留锁表抽象。
