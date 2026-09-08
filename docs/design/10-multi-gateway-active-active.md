# 10. 多网关多活（每导出一个活动网关）——设计与实现

> 状态：**已实现（2026-09-06）**，本册于 2026-09-08 按最终实现回写。本册原为 11 册：09 的实施
> 步骤（原 10 册）与本册的实施步骤（原 12 册，阶段 A1–E3）完成后均已撤下（每步实现记录见 git
> 历史；代码注释里的 `plan 12 A3`、`plan 10 D1` 等标签即指这两份已撤下的步骤文档），本册顺位
> 改为 10 册。未闭环项见
> [../toto/multi-gateway-active-active-followups.md](../toto/multi-gateway-active-active-followups.md)。
> 运维视角见 [../deployment.md](../deployment.md) §6，配置 / 指标 / ctl 见 08 册，状态层见 07 §7.5。
>
> 本册把 [09 册](09-multi-gateway-failover.md) §9.9 的一句展开成完整方案：在 09 主备接管的原语
> （`ClusterStore`、围栏租约、集群身份、后端接管钩子、per-fsid 会话 uuid）之上，把"一个集群一个
> 活动网关"扩展成 **每个导出一个活动网关**，用 RFC 8881 的 `fs_locations` 属性 + `NFS4ERR_MOVED`
> 把客户端引导到每个文件系统的**属主网关**。09 回答"主备怎么无感切换"，本册回答"怎么让 N 个
> 网关同时对外服务、各管一部分导出"。
>
> 前置：09 册全部前提（§9.2）、`ClusterStore` 抽象（§9.4）、集群身份（§9.3）、围栏与全局
> epoch（§9.5）、接管流程（§9.6）、后端接管钩子（§9.7）均已实现（见 09 §9.10 改动清单）。

## 10.1 目标与非目标

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
- **v3 多活透明化**：v3 无 `fs_locations`，见 §10.11 的明确边界。

## 10.2 与 09 主备的关系：身份模型的改变

这是多活相对主备最根本的一处不同，先讲清楚。

| | 09 主备（failover） | 10 多活（active-active） |
|--|--------------------|--------------------------|
| 客户端看到的服务器 | **一台**：所有网关同 `server_owner`，藏在**单一 VIP** 后 | 同一 `server_scope` 内的**多台** server：`server_owner.major_id` 按 node 派生，各网关是不同 server |
| 客户端如何找到"对的网关" | VIP 漂移（外部 HA），协议无感 | 协议引导：伪根 + `fs_locations` + `NFS4ERR_MOVED`（referral / migration） |
| 接管的语义 | 整台"服务器重启"，全局 epoch+1，客户端对全部 fs reclaim | 单个 **fs 迁移**，该 fs 在目标网关重启，客户端只对该 fs reclaim |
| 对外地址 | 一个 VIP | 入口地址（DNS-RR / 轻 VIP）只做伪根初次接触；每导出的数据面在其属主网关的**自有地址** `node_address` |

原因：多活要把不同导出定位到不同网关，就必须让客户端能区分"这个 fs 该找谁"。RFC 8881 的
`fs_locations` 天生做这件事——但它的语义是"这个 fs 在（可能不同的）server 上"，因此各网关必须
呈现**不同的 `server_owner`**（否则客户端把它们当同一 server 做 session trunking、以为状态互通）。
`server_scope` 仍取 `[cluster] id`（同一 scope 表示同一管理域，是 `SUPP_MOVED_MIGR/REFER` 生效
的前提，RFC 8881 §11.5）。

**实现**（`server/protocol_stack.cpp` 的 `derive_server_identity`）：failover 下 owner = scope =
`lightnfs-cluster:<id>`；active-active 下 owner = `lightnfs-cluster:<id>:<node>`、scope =
`lightnfs-cluster:<id>`；无集群时沿用 `<hostname>:<state_dir>`（`[server] server_owner/scope`
可覆盖）。引擎判断"这个 fsid 是不是我的"走 `core::FsOwnerView`（`core/fs_owner_view.hpp`）：
控制器发布 `fsid → FsOwner{role, node, address, fs_epoch}` 的 RCU 快照，引擎每次解析句柄读一次；
**未设置视图或未知 fsid 一律视为 Active**，所以单网关与 failover 的数据面路径与多活之前完全相同。

**两种模型共存**：`[cluster] mode = failover`（09，默认）保持不变；`mode = active-active`（本册）
启用 per-fsid 属主 + referral。一个部署二选一（`FsClusterController` 与 09 的 `ClusterController`
互斥，`daemon.cpp` 按 mode 只构造一个）。

## 10.3 所有权模型：每 fsid 一个围栏

`ClusterStore` 的键空间在 09 的文件旁加多活专属文件（`server/cluster_store.hpp` 顶部注释是权威）：

```
shared_dir/
  hmac.key                     # 句柄密钥：集群共享，不变（09）
  exports.<node>               # 导出表摘要：多活下 nodes 列表也进摘要（§10.10）
  epoch.<node>                 # 该网关自有 epoch：进程每次启动 +1；clientid/stateid/写验证器的来源（§10.5）
  nodes/<node>                 # "<node_address>\n"：该网关的地址登记，fs_locations 指向的就是它
  fence.<node>                 # "<expires_at_ms> <fsid>:<fs_epoch>[,<fsid>:<fs_epoch>…]\n"
                               #   该网关持有的全部 fsid，一条租约；空列表 = 心跳（不持任何导出也按周期写）
  fs/<fsid>/epoch              # 该导出的接管代数：属主每变更 +1（诊断 / 围栏代数，不进 stateid）
  fs/<fsid>/owner              # "<fs_epoch> <address> <node>\n"：当前属主（供非属主网关应答 fs_locations）
  fs/<fsid>/clients/<fnv64>    # 该导出的 reclaim 名单（该 fsid 的属主维护，co_ownerid 原文）
  fence.lock / epoch.lock / epoch.<node>.lock / fs/<fsid>/epoch.lock   # O_EXCL 串行化，陈旧锁按 pid+时间回收
```

- **没有 `fs/<fsid>/fence` 文件**：per-fsid 围栏是对全部 `fence.<node>` 记录的**派生视图**——
  `read_fs_fence(fsid)` 取存活且列出该 fsid 的记录，否则取最新的过期记录；`acquire_fs_fence`
  取围栏时把该 fsid 从其他节点的记录里剔除，保证一个导出永远只被一条记录列出；所有
  `fence.<node>` 的写都在同一把 `fence.lock` 下。接口保持 per-fsid 语义，实现层按节点批量。
- **续租批量化**：`renew_fences(node, ttl)` 每个 tick 重写一次自己的 `fence.<node>`，与持有多少
  导出无关（测试 `FsClusterController.RenewIsOneWritePerTick`），记录不存在时创建——不持任何
  导出的网关也在心跳。tick 周期 = `fence_lease`（默认 3s），围栏 ttl = 3 × `fence_lease`。
  续租连续失败 3 次 → 对自己持有的全部导出 Draining（不释放记录，避免删掉别人已接手的围栏）；
  续租失败的那个 tick 不做任何接管。
- 一个网关对 fsid F 是 **Active** ⟺ 它的 `fence.<node>` 存活且列出 F。**围栏、fs epoch、reclaim
  名单、grace 都按 fsid**，不再按网关。`FsClusterController`（`server/cluster_controller.*`）是
  "每 fsid 一个角色状态机 + 一条批量续租协程"：同一进程里对 fsid A 可能 Active、对 fsid B 是
  Remote。内部复用 09 的 `Role` 枚举（Standby = Remote / Activating / Active / Draining）；对引擎发布
  的视图 `FsRole` 四值 active / draining / remote / unowned，其中 **Activating 发布为 unowned**（客户端
  收 DELAY 而不是被误引到还没接完的网关）、进程启动读到第一次共享目录之前也是 unowned（网关不会
  在看清集群之前就开始服务）；ctl / 指标看到的是五个运维标签 active / activating / draining /
  remote / unowned（§10.13）。
- **属主选择不做选主**（延续 09"lightnfs 不选主，只提供接管动作与围栏"的原则）：每个
  `[[export]]` 配一个**优先级 node 列表** `nodes = ["gw1","gw2","gw3"]`，接管顺位由列表 +
  心跳决定（`our_turn`）：
  - `takeover = auto` 且自己在该导出的 `nodes` 里才是候选；不在列表里的网关永不自动接管，只能
    `ctl cluster takeover <fsid> --force`。
  - 列表里排在自己前面的每个网关：其 `fence.<node>` 心跳存活（空列表也算）→ 让位，它会自己接；
    没有任何记录 → 视为死，但**自己启动后一个 ttl 内**例外（同时启动的网关可能还没写第一次
    心跳，先让位）。
  - 一个导出无属主超过 **2 × ttl**（`stuck`）→ 忽略顺位直接接（前位活着却不接，比如它是
    `takeover = manual`，不能让导出一直没人服务）。
  - `takeover = manual` 时只认 `ctl cluster takeover <fsid>`；唯一例外是**计划内迁移的目标是
    自己**——那是运维在源端发起的，无视策略接管（§10.7）。
  - `ctl cluster standby <fsid>` 释放后该导出记 `held_off`：本网关不再自动抢回，直到别的网关
    持有过它或运维再次 `takeover`。

## 10.4 客户端引导：伪根 + fs_locations + NFS4ERR_MOVED

lightnfs 已有只读**伪文件系统**（`src/core/pseudofs.*`，fsid 0，从伪根 `/` 到每个导出点，
04 §4.3 / 07）。多活把它用作"分流入口"：

- **伪根在所有网关一致**（导出表摘要已在 09 §9.3 校验），任一网关都能应答伪根的
  `PUTROOTFH / LOOKUP / READDIR / GETATTR`。客户端经入口地址（DNS 轮询或轻量 VIP）落到**任一**
  网关，沿伪根向下走。
- **门禁在一处**：`Engine::resolve()` 的 `ownership_gate(fsid)`（`nfsv4/engine.cpp`）——凡是要把
  句柄解析到某个 fsid 的 op（READ / WRITE / ACCESS / LOOKUP / LOOKUPP / READDIR / SECINFO / OPEN /
  LOCK / SETATTR …）在**任何后端调用之前**按视图判定：Active → 放行；Draining / Remote →
  `NFS4ERR_MOVED`（10019，计 `lightnfs_v4_moved_total{fsid}`）；Unowned → `NFS4ERR_DELAY`
  （不计数）。别的网关铸的导出句柄拿到本网关，同样在 resolve 里直接 MOVED，不碰存储。所以
  MOVED 覆盖的是**全部** op，不只是设计初稿写的"状态类操作"——这是 RFC 8881 §11.10 要求的
  行为。不经 resolve 的 op 照常：`PUTFH / GETFH / SAVEFH / RESTOREFH`、`SEQUENCE`、`EXCHANGE_ID /
  CREATE_SESSION / DESTROY_* / BIND_CONN_TO_SESSION`、`RECLAIM_COMPLETE`、`TEST/FREE_STATEID`。
- **跨进一个导出（fsid F 的边界）时**：
  - `LOOKUP` 跨进非属主导出、以及 `/` 本身是他人导出时的 `PUTROOTFH`，都**成功**，cfh 置为该
    穿越节点的伪 fs 句柄（referral 点），不调后端 `root()`；导出回到本网关后同一个句柄由
    `resolve()` 映射到真实导出根，客户端缓存的句柄继续可用。
  - 伪目录的 `READDIR` 把非属主导出列成 referral 项：穿越句柄 + 缺席属性子集 + `rdattr_error =
    MOVED`（有属性被丢时）+ 属主 location（已知时）。
  - 对该句柄的 `GETATTR` 走**缺席 fs 应答**（`absent_attr_reply`）：只答 RFC 8881 §11.11.1 的子集
    `fs_locations`(24) / `fs_locations_info`(67) / `fsid`(8) / `rdattr_error`(11) /
    `mounted_on_fileid`(55)（与 knfsd 相同）；请求里既没有 `rdattr_error` 也没有两个
    `fs_locations` 属性、却要了子集之外的属性 → 整个 op 回 MOVED；否则 OK + 缩减后的掩码，
    丢了属性就置 `rdattr_error = MOVED`。Unowned 的导出连 `GETATTR fs_locations` 也回 DELAY。
  - `fs_locations` 编码（`nfsv4/attrs.cpp`）：`fs_root` = 该导出在伪 fs 里的路径分量（导出 `/`
    时为空）；`locations` 0 或 1 项，`server` = 属主 `node_address` 的**主机部分**（RFC 的
    server 字段是主机名，端口被剥掉，IPv6 去方括号；见收尾项 §2），`rootpath` = 同一 `fs_root`
    （各网关同树）；无属主 → 属性在、列表为空。`fs_locations_info` 是最小编码：`fli_flags = 0`、
    `fli_valid_for = lease`、一项 `fls_currency = -1`、`fls_info` 空。属主自己也应答 `fs_locations`
    （指向自己的 `node_address`），客户端可在迁移前探测。
  - 客户端按 RFC 8881 §11.10 处理 MOVED：对该文件系统重新 `EXCHANGE_ID`（到目标网关，得到
    目标网关的 clientid）+ `CREATE_SESSION`，然后 `OPEN(CLAIM_PREVIOUS)` / `LOCK(reclaim)` 重建
    该 fs 的状态。Linux NFSv4.1 客户端对 referral 是把被引导的 fs 挂成一个**子挂载**，clientid
    按目标网关独立——这正是我们要的（每 fs 的状态在其属主网关）。
- **属主变更后**（迁移或故障接管）：旧属主对 F 的后续请求回 `NFS4ERR_MOVED`（`fs_locations`
  指新属主）；旧属主交出 F 时 `StateMgr::release_fsid(F)` 丢弃本网关在 F 内的全部状态，并给
  **持有过 F 状态的每个客户端**记 `lease_moved_until = now + lease`，其后一个租约期内对该客户端
  的 `SEQUENCE` 应答置 **`SEQ4_STATUS_LEASE_MOVED`**（0x80，到时自动失效，不需显式清除）；只
  浏览过 F 的客户端不置位，由下一次 MOVED 得知。新属主为 F arm 一个 grace 窗口，名单内客户端
  reclaim。

## 10.5 每 fsid 的 grace 与 epoch（状态层改动）

这是多活对 `StateMgr`（07 册）最主要的改动，细节见 07 §7.5。

- **per-fsid grace**：`StateMgr` 的 grace 从一个全局字段变成 `fsid → GraceWindow{pending,
  listed, deadline, active}` 的窗口集合，**窗口 0 = 全部导出**（单网关重启与 failover 接管的既有
  全局 grace；多活下启动**不** arm 窗口 0，只有 `activate_fs(F)` 才 `load_grace_list(F)`）。API：
  `load_grace_list(fsid)` / `in_grace(fsid)`（窗口 0 或该导出窗口任一存活即为真）/ `end_grace(fsid)`
  （0 = 结束全部，`ctl grace-end` 用）/ `in_stable_list(fsid, owner)`（先查全局名单再查导出名单）/
  `grace_remaining_seconds(fsid)`；`Stats::fs_grace` 按导出报告，`cluster status` 的
  `grace_remaining_s` 即来自它。所有门禁点（OPEN reclaim / LOCK reclaim / 委托抑制 / 无状态写 /
  原生锁 reclaim 的 DELAY 重试）都用带 fsid 的重载。
- **名单按导出、写点后移**（`Config::per_fsid_reclaim`）：`StableStore` 三钩子 `load/put/erase`
  带 `fsid` 首参（0 = 全局名单，本机路径 `state_dir/fs/<fsid>/clients/`，集群路径
  `fs/<fsid>/clients/`）。多活下全局名单**不再在 CREATE_SESSION 写**；客户端在导出 F 内**首次铸出
  状态**（OPEN / LOCK / 委托）时 `put(F, owner)`，在 F 内最后一个状态销毁或客户端过期时
  `erase(F, owner)`。名单语义因此收紧为"在 F 持有状态、可能 reclaim 的客户端"。
- **`RECLAIM_COMPLETE` 仍是整 clientid 的**（与设计初稿不同）：引擎接受 `rca_one_fs = TRUE` 但
  不按 fs 处理，`note_reclaimed(owner)` 没有 fsid 维度，一次完成对该客户端所在的**所有窗口**
  生效。后果：客户端只发 per-fs 完成时该窗口不提前收窄、跑到期限；发全局完成则一次计入全部
  窗口。Linux 客户端在 referral 子挂载上发的是全局完成，实际影响是导出窗口最多多等到期限。
- **epoch / 身份仍按网关，不按 fsid**：多活下每个网关有自己的 `epoch.<node>`（**每次进程启动
  +1**，不同于 failover 的全局 `epoch` 每次接管 +1），进 clientid 高 32 位、stateid.other 前 4 字节；
  **写验证器额外混入 node 名**（`core::verifier_for_node`：高半 node、低半 epoch）——两个网关的
  epoch 数值可能相同，迁移后验证器必须变化，客户端才会重发 UNSTABLE 数据（v3 的验证器同样
  处理）。为什么不需要 per-fsid epoch 进编码：迁移让旧属主的 clientid/stateid 在新属主上**天然
  STALE**——新属主是不同 `server_owner`、独立的 clientid 空间，旧属主铸的它根本不认；客户端在新
  属主 `CLAIM_PREVIOUS` 铸出新 stateid。`fs/<F>/epoch` 只进围栏记录、owner 记录、视图与
  `lightnfs_cluster_fs_epoch{fsid}` 指标，**不进句柄 / stateid**；但交给后端 `takeover()` 与
  `takeover_hook` 的 `ClusterIdentity.epoch` / `LNFS_EPOCH` 是 **fs epoch**，随 `LNFS_FSID` 与
  `LNFS_REASON`（`takeover` | `migrate`）一起给出。
- **一个客户端多个 clientid**：客户端挂了落在不同网关的多个导出，就会持有多个 clientid（每
  目标网关一个）。这是 RFC 正确行为，Linux 客户端按 per-server 管理，无需特殊处理。

## 10.6 后端与一致性

- **单 fsid 单写者 → 无需跨网关锁仲裁**：因为一个 fsid 只有一个属主在写，该 fsid 的 share
  reservation / 字节锁仍是"网关本地"就够了（不像 09 主备里两网关可能同时在同一后端上锁）。
  09 §9.2 的 `kByteLocks` 前提在多活里可放宽为"每 fsid 单属主"；但**仍建议后端具备原生锁**，
  供属主故障接管时把 reclaim 锁下推到存储、并让新属主看见接管前的残留（§10.8）。
- **迁移/接管时的后端残留**：复用 09 §9.7 的 `Backend::takeover()` 钩子 + `[cluster]
  takeover_hook`，scope 到单 fsid（`prev_node` = 上一个持有者，`LNFS_REASON` 区分猝死接管与迁移）：
  - **CephFS 最干净**：09 §9.7 已实现的**每 fsid 一个会话 uuid**（`[export.cephfs] uuid` 默认
    `<cluster id>-<fsid>`）——新属主 `ceph_start_reclaim(<该 fsid 的 uuid>, RESET)` 只回收**该
    fsid** 的旧会话，不影响该网关正在服务的其他 fsid。多活与 CephFS 天生契合。只有 CephFS 覆盖
    了 `takeover()`，Gluster / Lustre 用默认空操作 + 外部 `takeover_hook`。
  - **GlusterFS / Lustre 隔离较弱**：libgfapi 的连接、Lustre 的客户端挂载是**整卷/整挂载级**，
    不是 per-fsid。单 fsid 迁移时若同一网关还在服务同卷的其他 fsid，`glfs_fini` / 客户端驱逐会
    波及整连接。因此**同一 Gluster `volume` / 同一 Lustre `mount` 上的所有导出必须列出完全相同的
    `nodes`**（同进退）——`core/config.cpp` 的 `validate_active_active` 按 `<backend>:<volume|mount>`
    分组，组内 `nodes` 不一致拒绝启动；`cluster_roll.sh` 逐个 `migrate` 时它们会依次到同一目标。
    只有启动期校验，没有运行期护栏。

## 10.7 计划内迁移（滚动升级 / 再平衡）

ctl 命令 `lightnfs-ctl cluster migrate <fsid> <node>`，**在当前属主上运行**
（`FsClusterController::request_migrate`）。流程（源网关 S、目标网关 T，对 fsid F）：

1. **S 校验**：本网关对 F 为 Active（否则 `not active here (role=…, owner=…)`）；T ≠ S、fsid 已知；
   T 在 `nodes/<T>` 登记过地址**且** `fence.<T>` 心跳存活（否则 `not a live gateway`）。
2. **S 对 F 进入 Draining** 并发布视图：引擎对 F 的请求开始回 `NFS4ERR_MOVED`。
3. **写 owner**：`fs/<F>/owner = {T, T 的地址, 当前 fs_epoch}`——在释放围栏**之前**写，保证 MOVED
   应答已能指向正确目标（避免"空窗指旧属主"）；写失败则回滚 Active。
4. **S 交出 F**：`release_fsid(F)`（丢弃 F 内全部 open / lock / 委托，不改名单、不下推原生 unlock、
   结束 F 的 grace、给持有过 F 状态的客户端置 LEASE_MOVED）→ `release_fs_fence(F)`；角色回
   Remote。**命令到此立即返回**（`migrate started: fsid= from= to=`）。没有在途请求静默期，
   状态即时丢弃——与"迁移 = 该 fs 重启"的语义一致。
5. **T 在下一个 tick 接管**：`migration_target()` 看到 owner 指向一个存活且 ≠ 上一持有者的节点、
   且无人持围栏 → **无视顺位、无视 `takeover = manual`** 接管，`reason = "migrate"`：读
   `fs/<F>/epoch`、`acquire_fs_fence(F, epoch+1)`、`bump_fs_epoch`、跑 F 的后端 `takeover()` +
   `takeover_hook`（`LNFS_PREV_NODE = S`）、`load_grace_list(F)`（arm F 的 grace）、写自己的
   owner 记录、开始对 F 服务。
6. **窗口内**：T 处于 Activating，对引擎发布为 unowned → 对 F 回 `DELAY`；其他网关的视图 Remote
   指向 T → 回 MOVED 指 T；除 T 以外的顺位网关看到 owner 指向存活的 T 时**主动让位**。因此
   **任何编排都必须轮询目标到 `role=active`**，而不是源端变 remote（收尾项 §3）。
7. 客户端：对 F 的下一个请求在 S 上收 `MOVED` / 在 `SEQUENCE` 收 `LEASE_MOVED` → 查 `fs_locations`
   → 到 T 重新 `EXCHANGE_ID/CREATE_SESSION` + reclaim（T 未接完前收 DELAY 重试）→ 一个 grace
   窗口内恢复；写验证器不同（§10.5），未提交写重发。

- **滚动升级一台网关**：`scripts/cluster_roll.sh evacuate <node> [--to <node>]` 对 `<node>` 上
  active / activating 的每个导出逐个 `migrate` 到 `--to`，或到该导出 `nodes` 里排在 `<node>` 之后的
  下一个活网关（环绕），每步轮询**目标**到 `role=active`，最后轮询到 `<node>` 不再服务任何导出
  （默认 30s，`--timeout` / `LNFS_ROLL_TIMEOUT`）；然后由运维重启 / 升级它（脚本**不**调
  `ctl drain`，进程退出时 `shutdown()` 自行释放围栏并留下空心跳记录）；`cluster_roll.sh restore
  <node>` 把 `nodes[0] == <node>` 且当前 remote 的导出从各自属主迁回（用 `--to` 指到第三方的
  导出不在 restore 范围内）。脚本全程走 `cluster status --json` + `cluster migrate`，需要涉及
  网关的 ctl 套接字（`--sockets gw1=…,gw2=…` / `LNFS_CTL_SOCKETS`）。全程无客户端重挂载。

## 10.8 故障接管（属主猝死）

与 09 §9.6 的 Activating 相同，但**按 fsid**、由备选网关自动触发：

1. 属主的 `fence.<node>` 心跳过期（ttl = 3 × `fence_lease` 内未续租）→ 按 §10.3 顺位轮到的网关对
   F 执行 Activating（`begin_activation`，`reason = "takeover"`）：`acquire_fs_fence(F, fs_epoch+1)`
   （顺手把 F 从死节点记录里剔除）、`bump_fs_epoch`、投递到主循环跑 F 的后端 `takeover()` +
   `takeover_hook`（清理猝死网关在存储侧对 F 的残留，§10.6，`LNFS_PREV_NODE` = 死节点）、
   `load_grace_list(F)`、写 `fs/<F>/owner`、开始对 F 服务。任一步失败 → 释放围栏、
   `fs_activation_failures_total{fsid}` +1、回 Remote 等下一 tick。
2. 其他网关的 tick 看到 F 的围栏换了持有者，视图 Remote → 对 F 一律回 `MOVED` 指新属主。
3. **一个网关猝死 = 它的每个 fsid 各自被各自的顺位网关接管**——若各导出的 `nodes` 列表下一位
   不同，接管负载自然分散到多台网关（再平衡），而不是全压到一台备机（测试
   `DeadNodeSpreadsAcrossSuccessors`）。
4. **围栏丢失**（自己 Active，但 F 不再出现在自己的存活记录里：被 `--force` 抢走、或续租中断
   超过 ttl）→ Draining（`fs_fence_lost_total{fsid}` +1，**不**释放记录，避免删掉别人已接手的围栏）
   → `release_fsid(F)` → Remote。若之后自己的存活记录仍列出 F（续租只是短暂中断）且
   `takeover = auto`、未 `held_off` → 立即重取。
- 脑裂与 09 同：per-fsid 围栏 + 墙钟 + NTP；两网关对同一 fsid 争抢时客户端在两个"重启"间来回
  reclaim，可由 `lightnfs_cluster_fs_epoch{fsid}` 与 `fs_fence_lost_total` 诊断，数据不静默错
  （09 §9.5）。

## 10.9 协议面新增（相对 09 / 现状）

- **GETATTR 支持 `fs_locations`（属性 24）与 `fs_locations_info`（属性 67）**：`src/nfsv4/attrs.cpp`
  按 §10.4 编码，`AttrSource{fs_root, owner, referrals}` 由引擎从伪 fs 路径与 `FsOwnerView` 填；
  `supported_attrs` **只在 `mode = active-active` 下宣告这两位**，单网关 / failover 静默丢弃请求里
  的这两位。
- **非属主 fsid 回 `NFS4ERR_MOVED`（10019）**：`src/nfsv4/engine.cpp` 的 `ownership_gate` 在
  `resolve()` 里对 Draining / Remote 回 MOVED、Unowned 回 DELAY（§10.4）；`util/errno.hpp` 增
  `Errno::kMoved`，`core/errmap.cpp` 映射并对所有 op 放行。缺席 fs 的 `GETATTR` / `READDIR` 项按
  RFC 8881 §11.11.1 子集应答。
- **`SEQUENCE` 应答置 `SEQ4_STATUS_LEASE_MOVED`（0x80）**：`release_fsid` 后一个租约期内，对持有过
  该导出状态的客户端置位（仅新请求路径，重放不置）。
- **`EXCHANGE_ID` 的 `eir_flags`**：`EXCHGID4_FLAG_USE_NON_PNFS` 之外，多活下加
  `SUPP_MOVED_REFER | SUPP_MOVED_MIGR`（0x3）；`server_owner.major_id` 按 node、`server_scope` 按
  集群（§10.2）。
- **伪根跨 fsid 的 `LOOKUP/READDIR/PUTROOTFH` 在非属主网关照常成功**，只是给出 referral 点与
  缺席属性子集（§10.4）。

## 10.10 配置

```toml
[cluster]
enabled      = true
id           = "3f9c…-uuid"
shared_dir   = "/mnt/cephfs/.lightnfs-cluster"
mode         = "active-active"        # failover(09 默认) | active-active
node         = "gw1"                  # 每台各自不同
node_address = "10.0.0.11:2049"       # 本网关对外自有地址：登记到 nodes/<node>，fs_locations 指向它
fence_lease  = "3s"                   # tick 周期；围栏 ttl = 3 × fence_lease
# role 必须为 auto（默认）；takeover = auto|manual 按导出生效；takeover_hook 可选

[[export]]
path   = "/export/a"
fsid   = 1
backend = "cephfs"
nodes  = ["gw1", "gw2", "gw3"]        # 属主优先级列表；第一个活的服务，其余按序接管
# [export.cephfs] uuid 默认 "<cluster id>-1"（09 §9.7，per-fsid 会话回收）

[[export]]
path   = "/export/b"
fsid   = 2
backend = "cephfs"
nodes  = ["gw2", "gw3", "gw1"]        # b 的属主优先 gw2 → 负载分摊
```

- 校验（`core/config.cpp` 的 `validate_active_active`，`mode = active-active` 时）：`[cluster] role`
  只能 `auto`（角色按导出）；`node_address` 形如 `host:port` / `[v6]:port`；每个 `[[export]]` 必须
  有非空 `nodes`，成员满足节点名规则（`[A-Za-z0-9_.-]{1,64}`）且列表内不重复；Gluster / Lustre
  后端"同卷 / 同挂载的导出 `nodes` 逐项相同"（§10.6）。**不**校验 `nodes` 成员是否是集群里已登记
  的节点，也不校验本网关自己是否出现在任何列表里（配置里写错节点名的后果是该位永远"无记录 =
  死"，被后位跳过）。非多活模式下 `node_address` / `nodes` 只告警忽略。
- `nodes` 进导出摘要 `exports.<node>`（`canonical_exports_text` 输出 `nodes=a,b,c`），各网关必须
  逐字相同，否则拒绝入集群；改 `nodes` 需重启。入口地址（DNS-RR / 轻 VIP）在部署侧配置，
  lightnfs 不管。

## 10.11 客户端兼容性与 v3 边界

- **Linux NFSv4.1 客户端**：支持 referral（`fs_locations`，`-o crossmnt` 自动建子挂载）与
  migration（`LEASE_MOVED` + `fs_locations`）。每个被引导的 fs 成为一个子挂载，clientid 按目标
  网关独立。这是多活的目标客户端。
- **不支持 referral 的老 v4 客户端**：收到 MOVED 会失败——文档要求这类客户端**直接挂到属主
  网关地址**（放弃分流的透明性），或整个部署退回 `mode = failover`（09 单 VIP 主备）。
- **v3 客户端**：v3 **无 `fs_locations`、无 referral**。多活对 v3 只能"每导出挂到其属主网关的
  地址"（运维/自动化按 `cluster status` 的 `owner=` / `address=` 维护导出→属主地址映射），或该
  导出退回主备单 VIP。**这是多活对 v3 的明确边界**：v3 得不到协议级分流。混挂 v3/v4 的部署，
  v3 侧要么固定挂属主、要么用 failover。

## 10.12 已知取舍与风险

- **单 fsid 仍单写者**：导出级并行，非文件级；热点单导出不因多活变快，需在建导出时拆分。
- **Gluster/Lustre 的 per-fsid 隔离弱**（§10.6）：单卷/单挂载的连接级锁使单 fsid 迁移波及整
  连接，靠"同卷同进退"配置校验规避；CephFS 的 per-fsid uuid 会话回收最干净。多活优先在 CephFS
  上落地。
- **客户端必须支持 referral/migration**：老 v4 客户端与全部 v3 走不了多活（§10.11）。
- **围栏仍是墙钟 + NTP**（同 09）；per-fsid 围栏靠 §10.3 的按节点批量记录，共享目录 IO 与导出数
  无关。
- **"暂无属主"的 fsid 回 DELAY 而不是 MOVED**：若某 fsid 无网关持围栏（全部备选都死、或接管者
  还在 Activating），状态类与 `GETATTR fs_locations` 一律回 `NFS4ERR_DELAY`，客户端按 JUKEBOX 语义
  重试直到某备选接管；伪目录 `READDIR` 里它仍以空 `locations` 的 referral 项出现。观测上
  `lightnfs_cluster_fs_owner{fsid}` **无样本**、`lightnfs_cluster_fs_role{fsid,role="unowned"} = 1`、
  `cluster status` 里 `role=unowned owner=none`，据此告警。
- **迁移是异步接管**：`cluster migrate` 在源端写 owner + 放围栏后立即返回，目标下一 tick 才接；
  编排要等目标 `role=active`（§10.7，收尾项 §3）。
- **`fs_locations` 只带主机名不带端口**（RFC 语义）：真实部署每网关不同 IP、标准端口无碍；同机
  多实例（本机验收）无法从 `fs_locations` 区分网关，只能行为性地验证（收尾项 §2）。
- **`RECLAIM_COMPLETE` 不按 fs**（§10.5）：导出窗口的提前结束依赖客户端发全局完成，否则跑到
  期限（默认 = lease，`[protocol] grace` 可调短）。
- **`nodes` 成员不与集群登记核对**（§10.10）：拼错的节点名静默被当作死节点。
- **迁移窗口的可用性**：迁移/接管期间该 fsid 有一个 grace 窗口，窗口内新建状态回 GRACE、读
  放行——单 fsid 短暂"半可用"，与 09 主备的接管窗口同量级，但只影响该 fsid。
- 小项：`SECINFO_NO_NAME(CURRENT_FH)` 不经 resolve，在缺席 fs 上返回 OK 而非 MOVED（`SECINFO` 与
  `PARENT` 形态则 MOVED）；`end_grace(fsid≠0)` 不清 `grace_any_` 快路径，下一次 `in_grace()` 扫描
  时自清。

## 10.13 实现阶段、验收与指标

| 阶段 | 交付 | 状态 |
|------|------|------|
| P1 | `ClusterStore` 多活键空间（`epoch.<node>` / `nodes/<node>` / 批量 `fence.<node>` / `fs/<fsid>/*`）；`StateMgr` grace 与名单改 per-fsid；`mode=active-active` 配置解析与校验 | ✅ 2026-09-06（原 12 册 A1–A3） |
| P2 | 多活 server 身份与 `eir_flags`；`fs_locations` / `fs_locations_info` 编码 + `FsOwnerView`；非属主 fsid 回 `NFS4ERR_MOVED` 与缺席属性应答 | ✅ 2026-09-06（B1–B3） |
| P3 | `FsClusterController` per-fsid 角色状态机 + 批量续租；按 `nodes` 顺位的自动接管（含 settling / stuck 规则）；`SEQ4_STATUS_LEASE_MOVED`；多活 ctl 与指标 | ✅ 2026-09-06（C1–C4） |
| P4 | `cluster migrate <fsid> <node>` 计划内迁移；`scripts/cluster_roll.sh evacuate/restore` | ✅ 2026-09-06（D1–D2） |
| P5 | 验收：`tests/accept_client.cpp` 的 `v4moved` 模式 + 三实例脚本 `scripts/accept_active_active_local.sh`；fake 演练；文档 | ✅ 2026-09-06（E1–E3），范围见下 |

- **本机验收实际覆盖**（`scripts/accept_active_active_local.sh`，Release 与 ASAN 各一轮，local 后端
  + `unsafe_skip_backend_checks`，三实例 loopback）：三网关**两导出**（fsid 1 `nodes=[gw1,gw2,gw3]`、
  fsid 2 `[gw2,gw3,gw1]`）——① status 断言属主分布；② `v4moved`：A 上 `eir_flags` 带 REFER|MIGR、
  他人导出有 `fs_locations` 且 READ 回 MOVED，B 上建 open/lock/未提交写后 `migrate 2 gw3`，B 上
  `SEQUENCE` 见 `LEASE_MOVED`、READ 回 MOVED，C 上同 co_ownerid `CLAIM_PREVIOUS` + `LOCK(reclaim)`
  成功、COMMIT 验证器与 B 不同、重发写字节级校验、`RECLAIM_COMPLETE` 后普通 OPEN 通过（提前出
  grace）；③ `cluster_roll.sh evacuate gw1` / `restore gw1`；④ `kill -9` gw1 后 gw2 在 10 ×
  `fence_lease` 内接管 fsid 1 并可写；⑤ **单网关退化**：一个网关三个导出全 `nodes=["gw1"]`，三行
  `role=active`、恰一个 `fence.gw1`（多活的单网关退化 = 单机行为，回归门）；⑥ 干净退出、无
  `level=error`、无 ASAN 报告。
- **未覆盖**（收尾项 §1）：真内核客户端（`mount -o vers=4.1` 走入口、子挂载、迁移中 fsx）与
  CephFS per-fsid uuid 回收的端到端验证——本机无 root / 无 Ceph 集群，留待 VM/CI。
- **单元测试**：`tests/test_cluster_controller.cpp`（单网关持全部 fsid、续租一次写、丢一个 fsid 只
  drain 那个、remote/unowned 与顺位、运维 takeover/standby、激活失败与 shutdown、顺位接管、猝死
  分散、stuck 跳过空闲前位、启动 settling、迁移经 owner 记录、目标死亡回退）、
  `tests/test_cluster_store.cpp`（批量围栏记录、per-fsid epoch/owner/clients/nodes 键）、
  `tests/test_ctl.cpp`（多活 ctl 全面、配置键校验）、`tests/test_metrics.cpp`（per-fsid 系列）、
  `tests/test_nfs4.cpp`（`fs_locations` 编码与宣告、导出边界 MOVED / unowned DELAY、`/` 被导出时的
  PUTROOTFH、per-node 身份）、`tests/test_state.cpp`（per-fsid grace、名单钩子、LEASE_MOVED、
  `release_fsid`）、`tests/test_gluster.cpp`（同卷同 `nodes` 校验）。`lnfs_accept_client` 与两个
  脚本不进 ctest。
- **指标**（08 册；`FsClusterController::append_metrics` + `server/metrics_providers.cpp`）：整机
  `lightnfs_cluster_node_epoch`、`lightnfs_cluster_migrations_total`（本网关作为源发起的迁移数）；
  每导出 `lightnfs_cluster_fs_role{fsid,role}`（one-hot：active / activating / draining / remote /
  unowned）、`lightnfs_cluster_fs_owner{fsid,node}`（值恒 1，无属主不出样本）、
  `lightnfs_cluster_fs_epoch{fsid}`、`lightnfs_cluster_fs_takeovers_total{fsid}`、
  `lightnfs_cluster_fs_fence_lost_total{fsid}`、`lightnfs_cluster_fs_activation_failures_total{fsid}`；
  引擎侧 `lightnfs_v4_moved_total{fsid}`（Draining / Remote 的 MOVED 与缺席属性应答计数，unowned 的
  DELAY 不计）。09 的整机 `lightnfs_cluster_role/_epoch/_fence_*` 系列在多活下不出样本。
- **ctl**（08 册）：`cluster status [--json]`（一行网关总览 + 每导出一行）、`cluster takeover <fsid>
  [--force]`、`cluster standby <fsid>`、`cluster migrate <fsid> <node>`；09 的无参形态在多活下答
  `fsid required`。

## 10.14 代码锚点

| 位置 | 改动 |
|------|------|
| `core/config.{hpp,cpp}` | `[cluster] mode / node_address`、`[[export]] nodes`（进导出摘要）、`validate_active_active`（含同卷同 `nodes`）、`cluster_active_active()` |
| `core/fs_owner_view.hpp` | `FsRole` / `FsOwner` / `FsOwnerView`（RCU 快照）、`address_host()` |
| `core/boot_epoch.{hpp,cpp}` | `verifier_for_node`：写验证器混入 node 名 |
| `server/cluster_store.{hpp,cpp}` | `epoch.<node>` / `nodes/<node>` / 批量 `fence.<node>`（`NodeFences`、`read_fs_fence` / `acquire_fs_fence` / `release_fs_fence` / `renew_fences` / `list_fences`）/ `fs/<fsid>/{epoch,owner,clients/}` |
| `server/cluster_controller.{hpp,cpp}` | `FsClusterController`：per-fsid 角色、`tick` / `our_turn` / `migration_target` / `begin_activation` / `begin_draining` / `request_takeover` / `request_release` / `request_migrate` / `publish` / `append_metrics` |
| `server/daemon.cpp` | 按 mode 构造 `FsClusterController`，`activate_fs` / `deactivate_fs` / `backend_takeover(fsid)` 钩子接到 `StateMgr` 与后端；多活下 `core->owners` 指向视图 |
| `server/takeover_hook.{hpp,cpp}` | `LNFS_FSID` / `LNFS_REASON` 环境变量 |
| `server/protocol_stack.cpp` | `derive_server_identity` 的 per-node owner；引擎 `referrals` 开关与 `set_owner_view`；多活下不 arm 全局 grace；验证器换 `verifier_for_node` |
| `server/ctl.cpp` | `cluster_fs_status` 与多活 `cluster` 分支 |
| `server/metrics_providers.cpp` | `append_v4_moved` |
| `state/state_mgr.{hpp,cpp}` | per-fsid `GraceWindow`、`StableStore` 钩子带 fsid、`per_fsid_reclaim` 写点、`release_fsid`、`lease_moved_until` 与 SEQUENCE 置位 |
| `nfsv4/attrs.{hpp,cpp}` | 属性 24 / 67 编码、`AttrSource`、`supported_attrs(referrals)` |
| `nfsv4/engine.{hpp,cpp}` | `ownership_gate` / `resolve` 的 MOVED、伪 fs 穿越点、`absent_attr_reply`、READDIR referral 项、`eir_flags`、`note_moved` / `moved_counts` |
| `util/errno.hpp`、`core/errmap.cpp` | `Errno::kMoved` → `NFS4ERR_MOVED` |
| `backend/cephfs/*` | per-fsid uuid（09 D2）沿用 |
| `tools/lightnfs_ctl.cpp`、`scripts/cluster_roll.sh`、`scripts/accept_active_active_local.sh`、`tests/accept_client.cpp` | ctl 客户端、滚动维护、三实例验收、`v4moved` 模式 |
