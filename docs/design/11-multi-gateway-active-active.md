# 11. 多网关多活（每导出一个活动网关）——设计方案

> 状态：**方案，未实现**。本册把 [09 册](09-multi-gateway-failover.md) §9.9 的一句展开成完整
> 方案：在 09 主备接管的原语（`ClusterStore`、围栏租约、集群身份、后端接管钩子、per-fsid
> 会话 uuid）之上，把"一个集群一个活动网关"扩展成 **每个导出一个活动网关**，用 RFC 8881
> 的 `fs_locations` 属性 + `NFS4ERR_MOVED` 把客户端引导到每个文件系统的**属主网关**。
> 09 回答"主备怎么无感切换"，本册回答"怎么让 N 个网关同时对外服务、各管一部分导出"。
>
> 前置：09 册全部前提（§9.2）、`ClusterStore` 抽象（§9.4）、集群身份（§9.3）、围栏与全局
> epoch（§9.5）、接管流程（§9.6）、后端接管钩子（§9.7）均已实现（见 09 §9.10 改动清单）。

## 11.1 目标与非目标

**目标**：

1. **导出级并行**：把 N 个导出（fsid）的读写负载分摊到 N 个网关，每个导出有且只有一个
   **属主网关**在写；不同导出可落在不同网关，集群总吞吐随网关数扩展。
2. **导出级故障接管**：一个网关猝死时，它属主的每个导出各自迁到各自的备选网关（可分散到
   多个网关，天然再平衡），而不是整机切到一台备机。
3. **计划内迁移**：把一个导出从当前属主平滑迁到指定网关（滚动升级、负载再平衡），客户端在
   一个 grace 窗口内跟随，不重挂载。

**非目标**：

- **单导出多写者**：一个 fsid 仍只有一个活动网关——多活是"导出级"并行，不是"文件级"并行；
  避免跨网关的 share reservation / 字节锁仲裁（那需要分布式锁管理器，是另一个量级的工程）。
  一个热点单导出不会因多活变快，应在建导出时按 fsid 拆分负载。
- **透明状态迁移**：不搬运 open/lock 状态（RFC 8881 §11.10.1 的"状态跟随迁移"）；沿用 09 的
  "迁移 = 该 fs 在目标网关重启 + 客户端 reclaim"语义。
- **跨网关 DRC / 会话槽复制**：接受重启级别的重放边界（同 09 §9.6）。
- **v3 多活透明化**：v3 无 `fs_locations`，见 §11.11 的明确边界。

## 11.2 与 09 主备的关系：身份模型的改变

这是多活相对主备最根本的一处不同，先讲清楚。

| | 09 主备（failover） | 11 多活（active-active） |
|--|--------------------|--------------------------|
| 客户端看到的服务器 | **一台**：所有网关同 `server_owner`，藏在**单一 VIP** 后 | 同一 `server_scope` 内的**多台** server：`server_owner.major_id` 按 node 派生，各网关是不同 server |
| 客户端如何找到"对的网关" | VIP 漂移（外部 HA），协议无感 | 协议引导：伪根 + `fs_locations` + `NFS4ERR_MOVED`（referral / migration） |
| 接管的语义 | 整台"服务器重启"，全局 epoch+1，客户端对全部 fs reclaim | 单个 **fs 迁移**，该 fs 在目标网关重启，客户端只对该 fs reclaim |
| 对外地址 | 一个 VIP | 入口地址（DNS-RR / 轻 VIP）只做伪根初次接触；每导出的数据面在其属主网关的**自有地址** |

原因：多活要把不同导出定位到不同网关，就必须让客户端能区分"这个 fs 该找谁"。RFC 8881 的
`fs_locations` 天生做这件事——但它的语义是"这个 fs 在（可能不同的）server 上"，因此各网关必须
呈现**不同的 `server_owner`**（否则客户端把它们当同一 server 做 session trunking、以为状态互通）。
`server_scope` 仍取 `[cluster] id`（同一 scope 表示同一管理域，是 `SUPP_MOVED_MIGR/REFER` 生效
的前提，RFC 8881 §11.5）。

**两种模型共存**：`[cluster] mode = failover`（09，默认）保持不变；`mode = active-active`（本册）
启用 per-fsid 属主 + referral。一个部署二选一。多活里若仍想给"某网关属主的那组 fsid"配整机
热备，可在 §11.13 之后叠加，但基础形态只有 referral 迁移一种机制，最简单。

## 11.3 所有权模型：每 fsid 一个围栏

`ClusterStore` 的键空间加 fsid 前缀（09 §9.4 已为此预留"演进到每导出一个角色时加 fsid 前缀"）：

```
shared_dir/
  hmac.key                      # 句柄密钥：集群共享，不变
  exports.<node>                # 导出表摘要：不变（§9.3 一致性校验）
  fs/<fsid>/fence               # 该导出的围栏租约 {node, epoch, expires_at}
  fs/<fsid>/epoch               # 该导出的接管代数（属主每变更 +1；仅诊断/围栏用，见 §11.5）
  fs/<fsid>/clients/<hash>      # 该导出的 reclaim 名单（该 fsid 的属主维护）
  fs/<fsid>/owner               # 当前属主 node + 其对外地址（供非属主网关做 fs_locations 应答）
```

- 一个网关对 fsid F 是 **Active** ⟺ 它持有 `fs/<F>/fence`。**围栏、epoch、reclaim 名单、grace
  都按 fsid**，不再按网关。`ClusterController`（09 §9.6）从"每进程一个角色"变成"每 fsid 一个
  角色状态机"——同一进程里对 fsid A 可能 Active、对 fsid B 是 Standby（转发方）。
- **属主选择不做选主**（延续 09"lightnfs 不选主，只提供接管动作与围栏"的原则）：每个
  `[[export]]` 配一个**优先级 node 列表** `nodes = ["gw1","gw2","gw3"]`。第一个活着且能拿到围栏
  的网关服务该 fsid；属主故障后，列表里下一个网关的围栏轮询发现过期 → 接管（复用 09 的
  auto-takeover 逻辑，scope 收到单 fsid）。`takeover = manual` 时只认 `ctl cluster migrate/takeover`。
- **续租批量化**（实现注）：per-fsid 围栏数 = 导出数，若每 fsid 各发一次续租，共享目录 IO 随
  导出数线性增长。优化：一个网关对它属主的所有 fsid **一条批量续租**（一个 `fence.<node>` 记录
  列出该 node 持有的 fsid 集与统一 expires_at），非属主网关按 node 记录反查每个 fsid 的属主。
  接口 `ClusterStore` 保持 per-fsid 语义，实现层批量。

## 11.4 客户端引导：伪根 + fs_locations + NFS4ERR_MOVED

lightnfs 已有只读**伪文件系统**（`src/core/pseudofs.*`，fsid 0，从伪根 `/` 到每个导出点，
04 §4.3 / 07）。多活把它用作"分流入口"：

- **伪根在所有网关一致**（导出表摘要已在 09 §9.3 校验），任一网关都能应答伪根的
  `PUTROOTFH / LOOKUP / READDIR / GETATTR`。客户端经入口地址（DNS 轮询或轻量 VIP）落到**任一**
  网关，沿伪根向下走。
- **跨进一个导出（fsid F 的边界）时**：
  - 若本网关持有 `fs/<F>/fence`（是属主）→ 正常服务（与今天完全相同）。
  - 否则 → 对该导出根 fh 的**状态类操作回 `NFS4ERR_MOVED`（10019）**，并对该 fh 的 `GETATTR`
    支持 `fs_locations`（属性 24）/ `fs_locations_info`（属性 67）：`fs_root` = 该导出在伪 fs 里的
    路径；`locations` = `fs/<F>/owner` 里记录的属主网关**自有地址** + 同一 `fs_root`。
  - 客户端按 RFC 8881 §11.10 处理 MOVED：对该文件系统重新 `EXCHANGE_ID`（到目标网关，得到
    目标网关的 clientid）+ `CREATE_SESSION`，然后 `OPEN(CLAIM_PREVIOUS)` / `LOCK(reclaim)` 重建
    该 fs 的状态。Linux NFSv4.1 客户端对 referral 是把被引导的 fs 挂成一个**子挂载**，clientid
    按目标网关独立——这正是我们要的（每 fs 的状态在其属主网关）。
- **属主变更后**（迁移或故障接管）：旧属主对 F 的后续请求回 `NFS4ERR_MOVED`（`fs_locations`
  指新属主），并在其后一个租约期内，对该客户端的 `SEQUENCE` 应答置
  **`SEQ4_STATUS_LEASE_MOVED`**（提示"你有 fs 迁走了，去查 `fs_locations`"）；新属主为 F
  armed 一个 grace 窗口，名单内客户端 reclaim。

## 11.5 每 fsid 的 grace 与 epoch（状态层改动）

这是多活对 `StateMgr`（07 册）最主要的改动。

- **per-fsid grace**：现在 grace 是全局的（07 §7.5：进程启动 → 全局进 grace）。多活里一个网关
  可能"对 fsid A 已 Active（不在 grace），此刻刚接管 fsid B（B 进 grace）"。因此 `StateMgr` 要按
  fsid 记录 grace 截止与 reclaim 名单：
  - 对 fsid F 内对象的 `OPEN(CLAIM_PREVIOUS)` / `LOCK(reclaim)`，用 **F 的名单 + F 的 grace** 门禁；
  - F 的 grace 内、对 F 的新建状态操作 → `NFS4ERR_GRACE`；对其他已 Active 的 fsid → 正常；
  - F 名单内客户端全部 `RECLAIM_COMPLETE` → F 提前出 grace（其他 fsid 不受影响）。
  - 实现：`StateMgr` 的 grace 状态从一个全局字段变成 `fsid → {deadline, reclaim_set}` 映射；
    07 §7.5 的既有 API（`load_grace_list` / `note_reclaimed` / `in_grace`）加一个 `fsid` 维度。
- **epoch/身份仍按网关，不按 fsid**：clientid 高 32 位、stateid.other 前 4 字节、写验证器仍取自
  **网关自有 epoch**（进程/整机接管代数，`state_mgr.cpp` 现有逻辑不动）。为什么不需要
  per-fsid epoch：迁移让旧属主的 clientid/stateid 在新属主上**天然 STALE**——新属主是不同
  `server_owner`、独立的 clientid 空间，旧属主铸的 clientid/stateid 它根本不认；客户端在新属主
  `CLAIM_PREVIOUS` 铸出新 stateid，未提交写因新属主写验证器不同而重发。`fs/<F>/epoch` 只做
  诊断与围栏代数（`lightnfs_cluster_fs_epoch{fsid}` 指标），**不进句柄/stateid 编码**。
- **一个客户端多个 clientid**：客户端挂了落在不同网关的多个导出，就会持有多个 clientid（每
  目标网关一个）。这是 RFC 正确行为，Linux 客户端按 per-server 管理，无需特殊处理。

## 11.6 后端与一致性

- **单 fsid 单写者 → 无需跨网关锁仲裁**：因为一个 fsid 只有一个属主在写，该 fsid 的 share
  reservation / 字节锁仍是"网关本地"就够了（不像 09 主备里两网关可能同时在同一后端上锁）。
  09 §9.2 的 `kByteLocks` 前提在多活里可放宽为"每 fsid 单属主"；但**仍建议后端具备原生锁**，
  供属主故障接管时把 reclaim 锁下推到存储、并让新属主看见接管前的残留（§11.8）。
- **迁移/接管时的后端残留**：复用 09 §9.7 的 `Backend::takeover()` 钩子，scope 到单 fsid：
  - **CephFS 最干净**：09 §9.7 已实现的**每 fsid 一个会话 uuid**（`[export.cephfs] uuid` 默认
    `<cluster id>-<fsid>`）——新属主 `ceph_start_reclaim(<该 fsid 的 uuid>, RESET)` 只回收**该
    fsid** 的旧会话，不影响该网关正在服务的其他 fsid。多活与 CephFS 天生契合。
  - **GlusterFS / Lustre 隔离较弱**：libgfapi 的连接、Lustre 的客户端挂载是**整卷/整挂载级**，
    不是 per-fsid。单 fsid 迁移时若同一网关还在服务同卷的其他 fsid，`glfs_fini` / 客户端驱逐会
    波及整连接。文档明示：**Gluster/Lustre 上一个网关不要同时属主"同一卷里的多个 fsid"**——
    要么整卷一个 fsid，要么整卷的所有 fsid 同属一个网关（同进退），才能干净迁移。

## 11.7 计划内迁移（滚动升级 / 再平衡）

新增 ctl 命令：`lightnfs-ctl cluster migrate <fsid> <target-node>`。

流程（源网关 S、目标网关 T，对 fsid F）：

1. **S 对 F 进入 Draining**：对 F 的**新**状态请求回 `NFS4ERR_MOVED`（`fs_locations` 已指向 T，
   见下），存量在途请求做完；停止对 F 的续租。
2. **写 owner**：`fs/<F>/owner = {T, T 的地址}`（S 在释放围栏前先写，保证 MOVED 应答已能指向
   正确目标，避免"空窗指旧属主"）。
3. **S 释放 `fs/<F>/fence`**。
4. **T 取 `fs/<F>/fence`、`epoch++`、arm F 的 grace、跑 F 的后端 takeover 钩子、开始对 F 服务**。
5. 客户端：对 F 的下一个请求在 S 上收 `MOVED` / 在 `SEQUENCE` 收 `LEASE_MOVED` → 查 `fs_locations`
   → 到 T 重新 `EXCHANGE_ID/CREATE_SESSION` + reclaim → 一个 grace 窗口内恢复。
- **滚动升级一台网关**：对该网关属主的每个 fsid 逐个 `migrate` 到备选网关 → 该网关无 fsid 后
  安全 `drain`/重启/升级 → 再逐个 `migrate` 回来。全程无客户端重挂载。

## 11.8 故障接管（属主猝死）

与 09 §9.6 的 Activating 相同，但**按 fsid**、由备选网关自动触发：

1. 备选网关（`nodes` 列表里 F 的下一顺位）的围栏轮询发现 `fs/<F>/fence` 过期（属主猝死、
   未续租）→ 对 F 执行 Activating：取围栏、`fs/<F>/epoch++`、arm F 的 grace、跑 F 的后端
   takeover 钩子（清理猝死网关在存储侧对 F 的残留，§11.6）、开始对 F 服务、写 `fs/<F>/owner`。
2. 其他网关的围栏轮询看到 `fs/<F>/owner` 变化，之后对 F 一律回 `MOVED` 指新属主。
3. **一个网关猝死 = 它的每个 fsid 各自被各自的下一顺位接管**——若各导出的 `nodes` 列表下一位
   不同，接管负载自然分散到多台网关（再平衡），而不是全压到一台备机（这是多活相对主备的
   一个附带好处）。
- 脑裂与 09 同：per-fsid 围栏 + 墙钟 + NTP；两网关对同一 fsid epoch 不同时客户端在两个"重启"
  间来回 reclaim，可由 `lightnfs_cluster_fs_epoch{fsid}` 诊断，数据不静默错（09 §9.5）。

## 11.9 协议面新增（相对 09 / 现状）

- **GETATTR 支持 `fs_locations`（属性 24）与 `fs_locations_info`（属性 67）**：`src/nfsv4/attrs.cpp`
  加这两个属性的编码；`fs_root` 用导出在伪 fs 里的路径，`locations` 从 `fs/<fsid>/owner` 生成。
- **非属主导出根返回 `NFS4ERR_MOVED`（10019）**：`src/nfsv4/engine.cpp` 在按 fh 定位到某 fsid 且
  本网关非其属主时，对状态类 op 回 MOVED；`GETATTR fs_locations` 仍放行（RFC 要求 MOVED 的 fs
  至少能查 `fs_locations`）。
- **`SEQUENCE` 应答置 `SEQ4_STATUS_LEASE_MOVED`**：属主变更后一个租约期内，对受影响客户端置位。
- **`EXCHANGE_ID` 的 `eir_flags` 置 `EXCHGID4_FLAG_SUPP_MOVED_REFER`（+ `_MIGR`）**：宣告支持
  referral / migration。
- **伪根跨 fsid 的 `LOOKUP/READDIR` 在非属主网关照常**（伪 fs 是所有网关一致的只读视图，
  crossing 到导出边界才触发 MOVED）。

## 11.10 配置

```toml
[cluster]
enabled      = true
id           = "3f9c…-uuid"
shared_dir   = "/mnt/cephfs/.lightnfs-cluster"
mode         = "active-active"        # 新键：failover(09 默认) | active-active
fence_lease  = "3s"
node_address = "10.0.0.11:2049"       # 新键：本网关对外自有地址，写入 owner 记录供 fs_locations

[[export]]
path   = "/export/a"
fsid   = 1
backend = "cephfs"
nodes  = ["gw1", "gw2", "gw3"]        # 新键：属主优先级列表；第一个活的服务，其余按序接管
# [export.cephfs] uuid 默认 "<cluster id>-1"（09 §9.7 已实现，per-fsid 会话回收）

[[export]]
path   = "/export/b"
fsid   = 2
backend = "cephfs"
nodes  = ["gw2", "gw3", "gw1"]        # b 的属主优先 gw2 → 负载分摊
```

- 校验（`mode = active-active` 时）：每个 `[[export]]` 必须有非空 `nodes`，其成员是集群里已知的
  node 名；`node_address` 非空；Gluster/Lustre 后端时校验"同卷的 fsid 的 `nodes` 列表一致"
  （§11.6 的隔离约束）。入口地址（DNS-RR / 轻 VIP）在部署侧配置，lightnfs 不管。

## 11.11 客户端兼容性与 v3 边界

- **Linux NFSv4.1 客户端**：支持 referral（`fs_locations`，`-o crossmnt` 自动建子挂载）与
  migration（`LEASE_MOVED` + `fs_locations`）。每个被引导的 fs 成为一个子挂载，clientid 按目标
  网关独立。这是多活的目标客户端。
- **不支持 referral 的老 v4 客户端**：收到 MOVED 会失败——文档要求这类客户端**直接挂到属主
  网关地址**（放弃分流的透明性），或整个部署退回 `mode = failover`（09 单 VIP 主备）。
- **v3 客户端**：v3 **无 `fs_locations`、无 referral**。多活对 v3 只能"每导出挂到其属主网关的
  地址"（运维/自动化维护导出→属主地址映射），或该导出退回主备单 VIP。**这是多活对 v3 的
  明确边界**：v3 得不到协议级分流。混挂 v3/v4 的部署，v3 侧要么固定挂属主、要么用 failover。

## 11.12 已知取舍与风险

- **单 fsid 仍单写者**：导出级并行，非文件级；热点单导出不因多活变快，需在建导出时拆分。
- **Gluster/Lustre 的 per-fsid 隔离弱**（§11.6）：单卷/单挂载的连接级锁使单 fsid 迁移波及整
  连接；CephFS 的 per-fsid uuid 会话回收最干净。多活优先在 CephFS 上落地。
- **客户端必须支持 referral/migration**：老 v4 客户端与全部 v3 走不了多活（§11.11）。
- **围栏仍是墙钟 + NTP**（同 09）；per-fsid 围栏数 = 导出数，用 §11.3 的批量续租控制共享目录 IO。
- **伪根"暂无属主"的 fsid**：若某 fsid 无网关持围栏（全部备选都死），伪根里对它回 MOVED 指
  "无属主"→ 客户端按 grace 语义重试，直到某备选接管；`lightnfs_cluster_fs_owner{fsid}` 指标
  暴露无属主态。
- **迁移窗口的可用性**：迁移/接管期间该 fsid 有一个 grace 窗口（默认 = lease），窗口内新建状态
  回 GRACE、读放行——单 fsid 短暂"半可用"，与 09 主备的接管窗口同量级，但只影响该 fsid。

## 11.13 实现阶段（若启动）

| 阶段 | 交付 | 依赖 |
|------|------|------|
| P1 | `ClusterStore` 键加 `fs/<fsid>/` 前缀 + 批量续租；`StateMgr` grace 改 per-fsid；`mode=active-active` 配置解析与校验 | 09 全部 |
| P2 | `fs_locations`/`fs_locations_info` 属性编码 + 非属主导出根回 `NFS4ERR_MOVED` + `eir_flags` REFER/MIGR | P1 |
| P3 | `ClusterController` 改 per-fsid 角色状态机 + per-fsid auto-takeover（备选按 `nodes` 顺序接管过期围栏）+ `SEQ4_STATUS_LEASE_MOVED` | P1 P2 |
| P4 | `ctl cluster migrate <fsid> <node>` 计划内迁移 + 滚动升级脚本 | P3 |
| P5 | 验收：三网关三导出，杀一个网关看其导出各自接管到不同网关；`migrate` 滚动；Linux 客户端跨导出不中断；CephFS per-fsid uuid 回收验证 | P1–P4 |

- **先单网关自测**：P1 落地后先让**一个网关持所有 fsid 的围栏**，行为应与单机等价（多活的
  单网关退化 = 主备的单网关路径），作为回归门。
- **指标**（08 册）：`lightnfs_cluster_fs_owner{fsid,node}`、`lightnfs_cluster_fs_epoch{fsid}`、
  `lightnfs_cluster_migrations_total`、`lightnfs_v4_moved_total{fsid}`。

## 11.14 与既有代码的接口预留

- `ClusterStore`（09 §9.4）：键空间加 `fs/<fsid>/` 前缀即可，接口方法加一个 `fsid` 维度或包一层
  "per-fsid store 视图"，POSIX 实现与"经后端 API 写导出树"实现都不受影响。
- `ClusterController`（09 §9.6，`server/cluster_controller.*`）：从"一个角色 + 一个围栏协程"变成
  "每 fsid 一个角色 + 一条批量续租协程"；`activate/deactivate` 钩子按 fsid 参数化。
- `StateMgr`（07）：grace/reclaim 名单加 fsid 维度；clientid/stateid/verifier 的 epoch 逻辑不动。
- `nfsv4/engine.cpp` + `attrs.cpp`：加 `fs_locations` 属性与 `NFS4ERR_MOVED` 的导出边界判定。
- 后端：CephFS 的 per-fsid uuid 已实现（09 §9.7 / D2）；Gluster/Lustre 加 §11.6 的"同卷同进退"
  校验。
