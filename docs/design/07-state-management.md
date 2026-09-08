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
    StateType     type;                // kOpen | kLock | kDeleg（读委托）
    uint32_t      seqid;               // stateid 版本
    ClientRef     client;
    ObjId         obj;
    // kOpen:
    OpenOwner     owner;  ShareMode access, deny;  backend::OpenPtr bopen;
    // kLock:
    LockOwner     lowner; StateRef parent_open;    // 区间在 LockMgr 表内
};

struct FileStateRec {                  // 冲突裁决：share reservation × 字节锁 × 读委托（冲突即 CB_RECALL）
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

- 启动：epoch++ → 读 clients/ 名单 → 进入 grace（`[protocol] grace`，`auto` = lease 90s；可设更短加快恢复）。
- grace 内：OPEN(CLAIM_PREVIOUS)/LOCK(reclaim) 仅接受名单内客户端（否则 RECLAIM_BAD）；普通新建状态操作 → GRACE；纯读操作（GETATTR/READ with 特殊 stateid）放行（实现选择：宽松放行读，兼容 v3 混布）。
- 提前结束：名单内客户端全部 RECLAIM_COMPLETE → 立即出 grace。
- **集群模式（`[cluster] enabled`，09 册）**：稳定存储从本机 `state_dir` 改到共享
  的 `shared_dir`——`hmac.key`、全局 `epoch`（每次接管 +1，非每次进程启动）、`clients/`
  reclaim 名单三者由 `ClusterStore` 读写，接管的网关据此进 grace 并接受故障网关客户端的
  reclaim（`state/state_mgr.cpp` 的名单读写走接口，本机实现即原 `state_dir` 语义）。
  另有 `fence`（围栏租约）与 `exports.<node>`（各节点导出摘要）也在 `shared_dir` 下。
- **多活（`[cluster] mode = active-active`，10 册 §10.5）：grace 与名单加 fsid 维度**。
  `StateMgr` 的 grace 从一个全局窗口变成按 fsid 的窗口集合（`state/state_mgr.hpp`：
  `load_grace_list(fsid)` / `in_grace(fsid)` / `end_grace(fsid)` / `in_stable_list(fsid, owner)` /
  `grace_remaining_seconds(fsid)`），**窗口 0 = 全部导出**，即单网关重启与 failover 接管的既有
  全局 grace；导出自己的窗口在本网关接管该导出时从该导出的名单 arm，只门禁该导出——
  其他已 Active 的导出照常服务。名单也按导出存放：`shared_dir/fs/<fsid>/clients/`（无钩子的
  本机路径 `state_dir/fs/<fsid>/clients/`），`StableStore` 三钩子 `load/put/erase` 带 `fsid` 首参
  （0 = 全局名单）。
  - **写点变化**（`Config::per_fsid_reclaim`，多活打开）：全局名单**不再在 CREATE_SESSION 写**；
    客户端在导出 F 内**首次铸出 stateid**（OPEN / LOCK / 委托）时 `put(F, owner)`，该客户端在 F
    内最后一个状态销毁或客户端过期时 `erase(F, owner)`（`ClientRec::fs_states` 按 fsid 计数）。
    名单语义因此收紧为"在 F 持有状态、可能 reclaim 的客户端"。failover / 单网关的 `fsid = 0`
    路径原样保留。
  - `RECLAIM_COMPLETE` 仍是整 clientid 的（引擎接受 `rca_one_fs` 但不按 fs 处理），一次完成对
    该客户端所在的所有窗口生效；`lightnfs-ctl grace-end` 结束全部窗口，`Stats::fs_grace` /
    `cluster status` 的 `grace_remaining_s` 按导出报告。
  - **属主权交出**（`release_fsid(F)`，迁移或围栏丢失时由 `FsClusterController` 调）：丢弃本网关
    在 F 内的全部 open / lock / 委托——**不**改名单（新属主据此 arm 它的 grace）、**不**下推原生
    unlock（存储侧残留由新属主的 takeover 钩子清理）、不发回调；F 的 grace 窗口结束；clientid 与
    会话保留；持有过 F 状态的每个客户端记 `ClientRec::lease_moved_until = now + lease`，其后一个
    租约期内 `SEQUENCE` 应答置 `SEQ4_STATUS_LEASE_MOVED`（0x80），提示客户端查 `fs_locations`
    到新属主 reclaim。epoch / clientid / stateid / 写验证器仍按网关（不按 fsid）：旧属主铸的
    stateid 在新属主上天然 STALE，无需 per-fsid epoch。
- v3 请求不受 grace 影响（v3 无状态）；同一后端同时被 v3/v4 客户端写时，grace 期间 v3 写与 v4 reclaim 锁理论上可竞争——v1 接受（不做 NLM，v3 侧本就无锁语义），文档明示。

## 7.6 字节锁表（网关内 LockMgr）

实现 05 分册 5.8 的接口（`state::GatewayLockMgr`，四个实现者之一；其余三个是 gluster/lustre/cephfs 的原生锁管理器）：

- 以 `{fsid, ObjId}` 为键、每文件一个有序区间列表（分片 + 普通互斥，纯表操作无 IO）；POSIX 合并/拆分语义；
- 非阻塞语义：冲突即返回 DENIED + 冲突者（v4 LOCK 不做服务器端排队，nfsv4/04 §4.5）；被拒者登记为等待者，区间释放时经回传通道发 CB_NOTIFY_LOCK（已实现，7.7）；
- 死锁检测不做（非阻塞锁无死锁）；
- `native_locks()` 存在的后端（gluster/lustre/cephfs）：**叠加**而非替换——本表仍负责 stateid/本地冲突/courtesy 回收/CB_NOTIFY_LOCK，`StateMgr::Config::native_locks` 钩子把每次 LOCK/LOCKU/LOCKT 额外下推到后端；后端拒绝（EAGAIN）则回滚网关授予并回 DENIED，后端错误回 DELAY/SERVERFAULT。

## 7.7 回传通道与读委托

v1 发布时不发委托：回传通道仅在 CREATE_SESSION 协商中应答、永不发 CB_COMPOUND；`SessionRec.back` 与 `bound_conns` 的结构已按可用设计，委托阶段只加发送侧——下文即该阶段的落地。

**实现更新（2026-08-28）**：发送侧已落地——CREATE_SESSION 的
CONN_BACK_CHAN / BIND_CONN_TO_SESSION(BACK/BOTH) 绑定连接为回传通道
（`transport::CbChannel`），服务器经其发 CB_COMPOUND{CB_SEQUENCE, CB_RECALL /
CB_NOTIFY_LOCK}（单 cb slot 串行、卡死一个租约判通道 down）；SEQ4_STATUS 在持有
可召回状态且无活通道时置 CB_PATH_DOWN。读委托为 `StateType::kDeleg`：只授予绑定了回传通道的会话（`[protocol] delegations` 可关）；
写打开、SETATTR、REMOVE/RENAME、匿名写等冲突操作触发 CB_RECALL 并回 DELAY，客户端
DELEGRETURN（或 CLAIM_DELEG_CUR_FH 转正）后重试；租约期内不归还由租约扫描器吊销
（`lightnfs_v4_deleg_revokes_total`）。CLAIM_DELEGATE_PREV 不支持——委托随重启消亡。

## 7.8 观测点

每张表导出指标：clients/sessions/opens/locks 计数、courtesy 数、grace 剩余、槽重放命中率、DRC 命中率、租约回收事件——是排"客户端挂死/状态泄漏"问题的第一现场（08 分册）。
