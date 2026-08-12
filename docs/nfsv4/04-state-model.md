# 4. NFSv4 状态模型

这是 v4 与 v3 的本质分野，也是实现工作量的核心。服务器需要维护一张多层状态表并处理其全部生命周期。

## 4.1 状态对象层次

```
clientid（客户端实例）
 ├─ 租约（整个 clientid 一个租约，所有状态共享）
 ├─ open-owner（客户端上的"打开主体"，通常对应一个进程/凭证）
 │    └─ open stateid（每个 <open-owner, 文件> 一个）
 │         ├─ share access / share deny 模式
 │         └─ lock-owner（通常对应进程）
 │              └─ lock stateid（每个 <lock-owner, 文件> 一个，下挂字节区间锁集合）
 ├─ delegation stateid（每个被委托文件一个）
 └─ (4.1) sessionid（会话，见 06 分册）
```

## 4.2 clientid 的建立（4.0 方式）

4.0 用两步握手（4.1 换成 EXCHANGE_ID/CREATE_SESSION，见 [06-sessions-v41.md](06-sessions-v41.md)）：

```
SETCLIENTID(nfs_client_id4{ verifier, id字符串 }, cb_client4 回调地址, ...)
   → clientid4（64 位，服务器分配）+ setclientid_confirm 验证值
SETCLIENTID_CONFIRM(clientid, confirm)
   → 确认生效；同 id 字符串的旧 clientid 的状态被丢弃
```

- `id` 字符串唯一标识客户端实例（Linux 用主机名+IP 等拼接）；`verifier` 是客户端重启计数——**verifier 变了 = 客户端重启了**，服务器确认新 clientid 时立即释放旧实例的全部状态（锁不再滞留，对比 NLM 的 NSM 通知机制）。
- 回调信息（cb_program + 地址）供服务器反向连接做委托召回；4.0 的痛点：NAT 后不可达 → 服务器测试回调失败 → 不发委托。

## 4.3 stateid：状态的通行证

```c
struct stateid4 {
    uint32_t seqid;        /* 该状态被修改的次数（每次 OPEN/DOWNGRADE/LOCK…递增） */
    opaque   other[12];    /* 服务器分配的状态标识 */
};
```

READ/WRITE/SETATTR(size)/LOCK 等 IO 类操作**必须携带 stateid**，服务器校验：

- `other` 查不到 → NFS4ERR_BAD_STATEID（或已删除 → NFS4ERR_STALE_STATEID 用于服务器重启前的旧状态）；
- `seqid` 比服务器记录的旧 → NFS4ERR_OLD_STATEID（客户端拿着过期版本，需用新版重试）；seqid=0 表示"不检查版本"（4.1 常用）；
- 状态所属 clientid 租约已过期 → NFS4ERR_EXPIRED。

**特殊 stateid**：

- 全 0（seqid=0, other 全 0）：**匿名 stateid**——无打开状态的 IO（如 NLM 时代语义、loopback 场景）。服务器应允许但受 share deny 约束。
- 全 1（seqid=0xffffffff, other 全 f）：**READ 旁路 stateid**——绕过强制锁/deny 检查读（谨慎支持）。

## 4.4 OPEN：最复杂的操作

```c
OPEN4args {
    seqid4        seqid;         /* 4.0 的 owner 序号（4.1 忽略） */
    uint32_t      share_access;  /* READ(1)/WRITE(2)/BOTH(3)；4.1 高位还编码委托意愿 */
    uint32_t      share_deny;    /* NONE(0)/READ(1)/WRITE(2)/BOTH(3) */
    open_owner4   owner;         /* { clientid, opaque owner<> } */
    openflag4     openhow;       /* NOCREATE 或 CREATE{UNCHECKED/GUARDED/EXCLUSIVE4(+4.1 EXCLUSIVE4_1)} */
    open_claim4   claim;         /* 见下 */
}
OPEN4res(成功) {
    stateid4      stateid;
    change_info4  cinfo;         /* 目录变更信息 */
    uint32_t      rflags;        /* 如 OPEN4_RESULT_CONFIRM(4.0)、RESULT_LOCKTYPE_POSIX */
    bitmap4       attrset;       /* CREATE 时实际应用的属性 */
    open_delegation4 delegation; /* NONE / READ / WRITE（见 05 分册） */
}
```

### claim 类型

| claim | 场景 |
|-------|------|
| CLAIM_NULL | 正常打开：CFH=目录，参数带文件名 |
| CLAIM_PREVIOUS | **服务器重启后宽限期内重建**：CFH=文件本身，声明"我崩溃前打开过" |
| CLAIM_DELEGATE_CUR | 凭当前持有的委托本地 open 后，向服务器登记 |
| CLAIM_DELEGATE_PREV | 客户端重启后找回重启前委托（罕用，4.1 有替代） |
| (4.1) CLAIM_FH | CFH=文件直接打开（不经目录） |
| (4.1) CLAIM_DELEG_CUR_FH / CLAIM_DELEG_PREV_FH | 上两者的 FH 变体 |

### share reservation（Windows 风格打开互斥）

OPEN 同时声明"我要读/写"（share_access）和"我拒绝别人读/写"（share_deny）。冲突的 OPEN 返回 NFS4ERR_SHARE_DENIED。UNIX 客户端 deny 恒为 NONE，但服务器必须实现检查（CIFS 网关、macOS 会用）。同一 open-owner 对同一文件多次 OPEN 会**合并**为一个 stateid（access/deny 取并集，seqid 递增）；OPEN_DOWNGRADE 收窄，CLOSE 释放全部。

### 创建语义

- UNCHECKED/GUARDED 同 v3；EXCLUSIVE4 同 v3 的 verifier 方案（存 verifier、重放判同）。
- 4.1 新增 **EXCLUSIVE4_1**：verifier + 属性一起带（解决 v3/4.0 排它创建不能设属性、需补 SETATTR 的缺陷）；服务器用 `suppattr_exclcreat` 属性声明支持哪些。
- OPEN 创建成功后 **CFH 自动切换为新文件**。

### 4.0 的 owner seqid 与 OPEN_CONFIRM（历史包袱，4.1 全废）

4.0 没有会话级重放保护，改在每个 open-owner/lock-owner 上维护 `seqid`：owner 的每个改状态操作（OPEN/CLOSE/LOCK/…）必须带 seqid=上次+1，服务器缓存该 owner 最后一个应答用于重放；乱序 → NFS4ERR_BAD_SEQID。首次使用某 open-owner 时服务器可要求 OPEN_CONFIRM（rflags 置 CONFIRM 位）确认往返。这套机制易错难调，是"直接实现 4.1"的最大论据。

## 4.5 字节区间锁

```
LOCK(locktype{READ_LT/WRITE_LT/READW_LT/WRITEW_LT}, reclaim,
     offset, length, locker{ 新 lock-owner(带 open_stateid) 或 既有 lock_stateid })
  → lock_stateid
  冲突 → NFS4ERR_DENIED + 冲突者信息（owner、区间）
LOCKT(测试)      LOCKU(解锁，区间需精确匹配已持有段的拆分规则同 POSIX)
```

- 锁挂在 **lock-owner**（对应客户端进程）下，首个 LOCK 从 open stateid 派生出 lock stateid。
- length=0 非法（NFS4ERR_INVAL）；length=~0（全 1）表示"到 EOF/无限"；offset+length 溢出 → NFS4ERR_INVAL。
- **W 后缀（READW_LT/WRITEW_LT）表示"愿意等待"**：4.0 服务器仍立即回 DENIED（客户端轮询）；4.1 可配合 CB_NOTIFY_LOCK 回调通知"可以来锁了"。协议没有服务器端排队授予的强制语义（对比 NLM 的 GRANTED 回调）。
- 锁与 share deny、委托的互斥：写锁/写 deny/写委托互相冲突，服务器要在同一张冲突表里统一裁决。
- 升降级：同 owner 的重叠 LOCK 按 POSIX 语义合并/拆分/升级。
- 4.0 需要 RELEASE_LOCKOWNER 清理不再用的 lock-owner（防 seqid 状态泄漏）；4.1 用 FREE_STATEID。

## 4.6 租约：状态的心跳

- 服务器给每个 clientid 一个租约（时长=属性 lease_time，服务器定，典型 60–90s）。**任何**携带该 clientid 有效状态的操作都隐式续租（4.0 另有显式 RENEW；4.1 由每个 COMPOUND 的 SEQUENCE 兼任）。
- 客户端义务：空闲时也要按 < lease_time 的周期发续租（Linux 按 lease_time 的 1/3 左右主动 RENEW/SEQUENCE）。
- 租约过期后服务器**可以**（不是必须）回收全部状态。礼貌性实践（courtesy client，Linux 5.19+ knfsd 实现）：无人冲突就先留着，一有冲突或资源紧张才回收——减少短暂网络抖动的伤害。回收后客户端操作收到 NFS4ERR_EXPIRED，只能向应用报错（锁语义已破坏）。

## 4.7 服务器重启恢复：宽限期与 reclaim

```
服务器重启 → 状态全丢（内存实现）→ 进入宽限期 grace（≥ lease_time）
客户端发现：任意操作返回 NFS4ERR_STALE_CLIENTID / STALE_STATEID / (4.1) BADSESSION
恢复流程（每客户端）：
  1. 重建 clientid（SETCLIENTID/…确认 或 EXCHANGE_ID+CREATE_SESSION）
  2. 对崩溃前每个打开文件：OPEN(CLAIM_PREVIOUS) 重建打开状态
     对每把锁：LOCK(reclaim=TRUE) 重建
  3. (4.1) RECLAIM_COMPLETE 宣布完毕
宽限期内：非 reclaim 的 OPEN/LOCK → NFS4ERR_GRACE（客户端等待重试）
宽限期结束：不再接受 reclaim（NFS4ERR_NO_GRACE），未重建的状态永久丢失
```

安全隐患与对策：宽限期只认"声明"，恶意/糊涂客户端可 reclaim 它从未持有的锁。严谨服务器在稳定存储记录"哪些客户端持有过状态"（Linux nfsdcltrack / nfsd recovery dir），重启后只允许名单内客户端 reclaim；进一步（RFC 8881 §8.4.3）记录锁细节可以在**无宽限期**下精确恢复（少见）。**lightnfs 至少要做客户端名单级的记录**，否则宽限期是敞开的。

另外：全部客户端都 RECLAIM_COMPLETE 后可提前结束宽限期（Linux 实现了）——重启恢复速度的关键优化。

## 4.8 客户端重启恢复

客户端重启后带**新 verifier**（4.0 SETCLIENTID / 4.1 EXCHANGE_ID）再次注册；服务器识别出同 id 不同 verifier ⇒ 释放旧实例全部状态。无需宽限期（那是服务器重启才要的）。

## 4.9 状态表实现要点（预览，详见 11 分册）

- 四张核心哈希表：clientid → 客户端记录；`other`(12B) → 各类 stateid 记录；(open-owner, fh) → open state；文件 → 锁表/委托表。
- `other` 建议编码：{ 服务器启动纪元(4B), 类型(1B), 计数器(7B) }——重启后旧 stateid 天然可识别为 STALE_STATEID。
- 所有状态变更走每文件锁 + 每客户端锁的两级锁序；租约检查在 COMPOUND 入口做一次。
- 内存状态 + 稳定存储的"客户端名单"是性价比最高的组合。
