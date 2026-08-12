# 6. NFSv4.1 会话机制

会话（session）是 4.1 最重要的结构性变化：在 clientid 之下、RPC 之上加了一层**可靠请求通道**，一举解决 4.0 的重放保护（seqid 迷宫）、回调可达性（NAT）、DRC 不可靠三大痛点。

## 6.1 建立流程

```
1. EXCHANGE_ID(client_owner{ co_verifier, co_ownerid }, flags, state_protect, ...)
     → clientid4, sequenceid, 服务器能力 flags（EXCHGID4_FLAG_USE_NON_PNFS/…）
   （取代 SETCLIENTID；co_verifier 变化=客户端重启，语义同 4.0）

2. CREATE_SESSION(clientid, sequenceid,
     flags{ SESSION4_PERSIST | SESSION4_BACK_CHAN | SESSION4_RDMA },
     fore_chan_attrs{ ca_maxrequestsize, ca_maxresponsesize,
                      ca_maxresponsesize_cached, ca_maxoperations,
                      ca_maxrequests /* = 槽数 */ , ... },
     back_chan_attrs{...}, cb_program, sec_parms)
     → sessionid4(16B), 确认后的通道参数
   （CREATE_SESSION 本身用 clientid+sequenceid 做重放保护——鸡生蛋问题的解）

3. 此后每个 COMPOUND 第一个操作必须是 SEQUENCE（例外仅：EXCHANGE_ID、
   CREATE_SESSION、DESTROY_SESSION、BIND_CONN_TO_SESSION、DESTROY_CLIENTID）
```

## 6.2 槽表与精确一次语义（EOS）

会话前向通道有 `ca_maxrequests` 个**槽（slot）**，编号 0..N-1。每个槽是一个独立的"停等信道"：

```c
SEQUENCE4args {
    sessionid4  sa_sessionid;
    sequenceid4 sa_sequenceid;   /* 该槽上的序号，每次+1 */
    slotid4     sa_slotid;       /* 用哪个槽 */
    slotid4     sa_highest_slotid; /* 客户端当前用到的最高槽（供服务器收缩） */
    bool        sa_cachethis;    /* 要求服务器缓存本应答（供重放） */
}
```

服务器对每个槽记录 `(最后 seqid, 缓存的应答)`，收到请求时三分支：

| 收到的 seqid | 处理 |
|--------------|------|
| = 记录+1 | 新请求，正常执行，执行后更新槽（若 cachethis 则缓存全应答） |
| = 记录 | **重放** → 直接回缓存的应答（没缓存 → NFS4ERR_RETRY_UNCACHED_REP） |
| 其他 | NFS4ERR_SEQ_MISORDERED |

效果：

- **协议级精确一次语义**：v3 时代"尽力而为的 DRC"（见 nfsv3 分册 9.2）变成了双方协商、有硬边界的契约。非幂等操作的重放问题从根上解决。
- 应答缓存大小可控：槽数 × ca_maxresponsesize_cached，服务器在 CREATE_SESSION 时把参数压到自己能承受的值即可（客户端必须遵守）。
- 槽同时是**流控**：未完成请求数 ≤ 槽数。服务器可通过 SEQUENCE 应答里的 `sr_highest_slotid`/`sr_target_highest_slotid` 动态收缩扩张并发窗口（还有 CB_RECALL_SLOT 强收）。
- 4.0 的 open-owner seqid、OPEN_CONFIRM、RELEASE_LOCKOWNER、RENEW 全部因此废除；**SEQUENCE 兼任续租**（每个 COMPOUND 都在续租）。

实现注意：

- 槽表按 (sessionid, slotid) 索引，应答缓存只需缓存 `sa_cachethis=TRUE` 的（Linux 客户端对改状态操作会置位）；未缓存时对重放回 RETRY_UNCACHED_REP 是合法的。
- SEQUENCE 应答的 `sr_status_flags` 是服务器给客户端递条子的地方：SEQ4_STATUS_CB_PATH_DOWN（回传断了）、…_LEASE_MOVED、…_RESTART_RECLAIM_NEEDED 等，客户端每次都会看。

## 6.3 连接与会话的关系（trunking）

- 一条会话可绑多条 TCP 连接（**session trunking**）：客户端对同一服务器开多条连接并行发请求（`nconnect=N` 挂载选项），BIND_CONN_TO_SESSION 把连接绑进会话。槽表全会话共享。
- 一个 clientid 也可有多个会话（**clientid trunking**，多地址服务器）。
- EXCHANGE_ID 应答里的 so_major_id/server_scope 让客户端判断"两个地址是不是同一台服务器"，决定能否 trunk。
- 连接断开不丢会话状态：重连 + BIND_CONN_TO_SESSION（或直接在新连接上发 SEQUENCE，若服务器允许）即恢复；会话本身靠租约存活。

## 6.4 会话与状态恢复的交互

- 服务器重启：会话与全部状态丢失。客户端 SEQUENCE 收到 NFS4ERR_BADSESSION → EXCHANGE_ID 发现 clientid 也没了（NFS4ERR_STALE_CLIENTID）→ 走完整重建 + 宽限期 reclaim（见 04 分册 4.7），最后 **RECLAIM_COMPLETE**。
- 仅会话丢失（服务器裁剪）：BADSESSION 后 CREATE_SESSION 重建即可，锁/打开状态还在。
- DESTROY_SESSION/DESTROY_CLIENTID：客户端 umount 时的礼貌清理。

## 6.5 状态保护（state protection，可选加固）

EXCHANGE_ID 的 state_protect 参数防"别人冒充我毁我状态"：

- SP4_NONE：不设防（AUTH_SYS 环境的现实选择）；
- SP4_MACH_CRED：关键操作（DESTROY_CLIENTID 等）必须用注册时的 GSS 机器凭证；
- SP4_SSV：用会话生成的共享密钥（SET_SSV 维护）做轻量 GSS——规范复杂，主流实现基本不用。

lightnfs：支持 SP4_NONE 即可起步。

## 6.6 持久会话与 RDMA（可以忽略的角落）

- SESSION4_PERSIST 请求"应答缓存落稳定存储"（服务器重启后仍能重放）——几乎没有实现真做，拒绝该 flag 合法。
- RFC 8267 定义 RPC-over-RDMA 承载，超出 lightnfs 范围。

## 6.7 最小合规实现清单（4.1 服务器）

1. EXCHANGE_ID / CREATE_SESSION / DESTROY_SESSION / DESTROY_CLIENTID / BIND_CONN_TO_SESSION；
2. SEQUENCE：槽表、seqid 三分支、应答缓存（cachethis）、续租、status_flags 常置 0；
3. RECLAIM_COMPLETE（哪怕只是记个标志）；
4. FREE_STATEID / TEST_STATEID（客户端恢复路径会用）；
5. 回传通道可以宣告支持但从不发回调（不发委托就用不上）；SET_SSV、GET_DIR_DELEGATION、pNFS 系列全部 NFS4ERR_NOTSUPP。

这套骨架 + 04 分册的状态模型，就是"能让 Linux 客户端以 vers=4.1 稳定挂载使用"的完整下限。
