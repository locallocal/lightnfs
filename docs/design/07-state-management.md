# 7. 状态管理

协议语义依据 nfsv4 分册 [04-state-model.md](../nfsv4/04-state-model.md)、[06-sessions-v41.md](../nfsv4/06-sessions-v41.md)。本文写数据结构与并发实现。范围：v4.1/4.2 状态 + v3 DRC（DRC 结构已在 03 分册 3.7，此处不重复）。v3 锁（NLM）不做（决策 D8/nfsv3 分册 6.6 选项 1）。

## 7.1 数据结构总览（对应 nfsv4/11.3 的建议结构）

```cpp
namespace lnfs::state {

class StateMgr {                       // 全局单例，内部全部 Sharded
    Sharded<ClientTable>   clients_;   // clientid → ClientRec
    Sharded<SessionTable>  sessions_;  // sessionid(16B) → SessionRec
    Sharded<StateTable>    states_;    // stateid.other(12B) → StateRec
    Sharded<FileStateIdx>  files_;     // ObjId → FileStateRec（share/lock 冲突裁决入口）
    LeaseQueue             leases_;    // 最小堆：到期扫描
    GraceCtl               grace_;
    LockMgr                locks_;     // 网关内字节锁表（backend::LockMgr 同接口，5.8）
};

struct ClientRec {
    clientid4     id;                  // {boot_epoch(32) | counter(32)}
    NfsClientOwner co_owner;           // EXCHANGE_ID 的 co_ownerid + verifier
    TimePoint     lease_expiry;
    SmallVec<SessionRef, 2> sessions;
    bool          reclaim_complete;
    bool          in_stable_list;      // 已写入宽限期名单
};

struct SessionRec {
    sessionid4    id;
    ClientRef     client;
    ChanAttrs     fore, back;
    std::vector<Slot> slots;           // Slot{seq, in_flight, cached_reply(SendBuf)}
    SmallVec<ConnRef, 4> bound_conns;
};

struct StateRec {                      // other = {boot_epoch(4B)|type(1B)|counter(7B)}
    StateType     type;                // kOpen | kLock | kDeleg(预留) | kLayout(预留)
    uint32_t      seqid;               // stateid 版本
    ClientRef     client;
    ObjId         obj;
    // kOpen:
    OpenOwner     owner;  ShareMode access, deny;  backend::OpenPtr bopen;
    // kLock:
    LockOwner     lowner; StateRef parent_open;    // 区间在 LockMgr 表内
};

struct FileStateRec {                  // 冲突裁决：share reservation × 字节锁 ×（未来）委托
    SmallVec<StateRef, 4> opens;
    LockSet               locks;
};
}
```

- `stateid.other` 编码含 boot_epoch：重启后旧 stateid 查表前即判 STALE_STATEID（nfsv4/11.3）。
- clientid 同理含 epoch → STALE_CLIENTID 零成本判定。
- OPEN 合并（同 owner 同文件多次 OPEN）：`FileStateRec.opens` 里按 owner 查到既有 StateRec → 并集 access/deny、seqid++，不新建。

## 7.2 锁序与并发

全局锁顺序（02 分册 2.4 规约的实例化）：

```
① session 片锁（SEQUENCE 槽校验，最先、最短）
② client 片锁（租约/owner 表）
③ core ObjLock（文件语义锁）
④ files_/states_ 片锁（状态表本体，最内层、只做表操作）
```

- 全部片锁是 AsyncMutex，持锁不做后端 IO（唯一例外：CLOSE 释放 backend OpenPtr 的析构可能关 fd → 移出临界区后异步执行）。
- 租约续期无锁化：`lease_expiry` 用 atomic 存 coarse 时间戳，SEQUENCE 快路径只做 store；LeaseQueue 惰性重排。

## 7.3 SEQUENCE 快路径

每个 v4 请求都过 SEQUENCE，性能预算 = 哈希一次 + 片锁一次：

```
sessionid 查表 → slot 边界检查(BADSLOT/BAD_HIGH_SLOT)
→ seq 三分支：
   new(=last+1): 标 in_flight，放锁执行；完成回填 cached_reply(若 cachethis)
   replay(=last): 有缓存 → 直接发缓存；无 → RETRY_UNCACHED_REP
   else: SEQ_MISORDERED
→ in_flight 的重复到达：等待其完成后按 replay 处理（AsyncCondVar）
```

槽缓存内存预算：`slots × ca_maxresponsesize_cached`；默认 32 槽 × 8KiB 上限（改状态操作的应答都小；READ 等大应答客户端不会 cachethis，若 cachethis 且超限 → REP_TOO_BIG_TO_CACHE）。

## 7.4 租约与回收

- 到期扫描协程：每秒检查 LeaseQueue 顶部；到期 client 进入 **courtesy 状态**（标记不回收，nfsv4/04 §4.6）：
  - 与新请求冲突（share/lock 冲突检查时发现对方是 courtesy）→ 立即回收该 client 全部状态，冲突请求放行；
  - courtesy 超过 `lease_time × N`（默认 24×）→ 无条件回收。
- 回收动作：逐 StateRec 释放（backend OpenPtr 析构、锁表清除、files_ 反引用摘除）→ ClientRec 删除 → 稳定名单移除。

## 7.5 宽限期与持久化

**唯一的稳定存储状态**（nfsv4/04 §4.7 红线 + 11.3）：

```
state_dir/
  boot_epoch            # 每次启动 +1；同时是 write verifier 与 stateid epoch 来源
  hmac.key              # 句柄 HMAC 密钥（04 分册 4.3）
  clients/<hash(co_ownerid)>   # 内容：co_ownerid 原文；EXCHANGE_ID 确认后写入，
                               # 客户端状态全清/过期回收后延迟删除
```

- 启动：epoch++ → 读 clients/ 名单 → 进入 grace（时长 = lease_time，默认 90s）。
- grace 内：OPEN(CLAIM_PREVIOUS)/LOCK(reclaim) 仅接受名单内客户端（否则 RECLAIM_BAD）；普通新建状态操作 → GRACE；纯读操作（GETATTR/READ with 特殊 stateid）放行（实现选择：宽松放行读，兼容 v3 混布）。
- 提前结束：名单内客户端全部 RECLAIM_COMPLETE → 立即出 grace。
- v3 请求不受 grace 影响（v3 无状态）；同一后端同时被 v3/v4 客户端写时，grace 期间 v3 写与 v4 reclaim 锁理论上可竞争——v1 接受（不做 NLM，v3 侧本就无锁语义），文档明示。

## 7.6 字节锁表（网关内 LockMgr）

实现 05 分册 5.8 的接口（v1 唯一实现者）：

- per-ObjId 区间树（boost::icl 风格的手写 interval map）；POSIX 合并/拆分语义；
- 非阻塞语义：冲突即返回 DENIED + 冲突者（v4 LOCK 不做服务器端排队，nfsv4/04 §4.5）；CB_NOTIFY_LOCK 留待委托阶段一并做；
- 死锁检测不做（非阻塞锁无死锁）；
- `native_locks()` 存在的后端（未来 Lustre/Gluster）：StateMgr 改持后端 LockMgr，本表退化为 owner 簿记。

## 7.7 回传通道（占位）

v1 不发委托 → 回传通道仅在 CREATE_SESSION 协商中应答（接受客户端参数、SEQ4_STATUS 不置 CB_PATH_DOWN、永不发 CB_COMPOUND）。`SessionRec.back` 与 `bound_conns` 的结构已按可用设计，委托阶段（路线图 M8）只加发送侧。

**实现更新（2026-08-28，plan doc 10 §5.2）**：发送侧已落地——CREATE_SESSION 的
CONN_BACK_CHAN / BIND_CONN_TO_SESSION(BACK/BOTH) 绑定连接为回传通道
（`transport::CbChannel`），服务器经其发 CB_COMPOUND{CB_SEQUENCE, CB_RECALL /
CB_NOTIFY_LOCK}（单 cb slot 串行、卡死一个租约判通道 down）；SEQ4_STATUS 在持有
可召回状态且无活通道时置 CB_PATH_DOWN。读委托为 `StateType::kDeleg`：授予/冲突
召回/DELEGRETURN/超时吊销见 plan doc 10 §5.2 的落地说明。

## 7.8 观测点

每张表导出指标：clients/sessions/opens/locks 计数、courtesy 数、grace 剩余、槽重放命中率、DRC 命中率、租约回收事件——是排"客户端挂死/状态泄漏"问题的第一现场（08 分册）。
