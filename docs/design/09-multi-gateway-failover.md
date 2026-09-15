# 9. 多网关无感故障切换（共享后端）——设计与实现

> 多个 lightnfsd 网关共挂同一个共享后端（GlusterFS / Lustre / CephFS，或任何满足 §9.2 前提的
> 后端）时，一个网关故障后客户端切到另一个网关**不重挂载、不重建应用状态**：打开的文件、
> 字节锁、未提交的写都由协议机制恢复。本册是 `[cluster] mode = "failover"`（默认形态）：
> 同一时刻一个网关对外服务，故障后另一个接管。"每个导出一个属主网关、N 台同时服务"的多活
> 形态是它的演进，见 [10 册](10-multi-gateway-active-active.md)（§9.9）；两者是同一
> `[cluster]` 段的互斥取值。配置 / 指标 / ctl 见 [08 册](08-config-observability.md)，部署见
> [../deployment.md](../deployment.md) §5，把导出表从本地文件上移到共享清单见
> [11 册](11-shared-export-catalog.md)。未闭环的取舍与发布前门槛见
> [../toto/multi-gateway-failover-followups.md](../toto/multi-gateway-failover-followups.md)。

## 9.1 问题：切换后客户端看到的是"另一台服务器"

单机形态下的网关（05 §5.8、06、07 §7.6）已经具备"多台同时挂同一后端"的一半条件，缺的那半
正是本册补的：

| 后端 / 协议已经提供 | 多网关下暴露的缺口 |
|--------------------|-------------------|
| 集群后端的句柄跨网关稳定（GFID / FID / vinodeno，`kStableHandles`） | 句柄 HMAC 密钥按 `state_dir/hmac.key` 每网关各自生成 → 另一网关回 BADHANDLE |
| 字节锁下推到存储（`kByteLocks`），网关之间互斥 | v4 状态（clientid / 会话 / open / lock / 委托）只在网关内存，reclaim 名单只在本机 `state_dir/clients/` |
| CephFS 有原生 change（跨网关 CTO） | `server_owner` / `server_scope` 按主机名 + `state_dir` 派生 → 客户端把两台网关当**两台服务器** |
| 重启后 grace + CLAIM_PREVIOUS 的完整 reclaim 链（07 §7.5） | boot epoch 每网关独立 → 两台网关可能用相同 epoch，旧 stateid 不会被判 STALE |
| 读委托 + 回传通道 | 故障网关在存储侧的锁 / 会话残留到超时前，新网关 reclaim 时下推被拒 |

**核心思路：把"切换到另一台网关"变成客户端早已会处理的"同一台服务器重启"**（RFC 8881
§8.4.2.1）——统一身份 + 共享 reclaim 名单 + 全局单调 epoch + 接管即进 grace。这与 knfsd 共享
`/var/lib/nfs`、Ganesha 集群 recovery backend 的做法同构，Linux 客户端的恢复路径是现成的。

## 9.2 前提与范围

**前提**（不满足则本方案不适用，配置校验期拒绝）：

1. 所有网关导出**同一棵树**：`[[export]]` 的 `path` / `fsid` / `backend` 与后端子表逐项一致
   （校验方式见 §9.3；导出表集中到共享清单时由单一事实来源保证，11 册）；
2. 后端置位 `kStableHandles`（local 后端仅内核句柄模式满足，且各网关要看到同一挂载——
   本册面向集群后端，local 只在共享块设备 + 集群文件系统上有意义）；
3. 后端置位 `kByteLocks` 并开启 `native_locks`——否则两网关的锁互不可见，"无感"只剩
   open 状态；
4. 客户端经**单一服务地址**访问（VIP / DNS 单名），地址漂移由外部 HA（keepalived、
   pacemaker、云 LB）完成；lightnfs 不做仲裁选主，只提供"接管"动作与围栏。

前 3 条在配置校验期检查（`core/config.cpp`；后端能力位在后端构造之后查，`--check-config`
同样执行），`unsafe_skip_backend_checks` 仅供测试把能力不达标降级为告警。

**范围**：

- **主备 + 接管**（本册）：同一时刻一个网关对外服务；故障后另一网关接管，v3 客户端无感，
  v4.1 客户端在一个 grace 窗口内 reclaim 全部 open / lock。
- **非目标**：跨网关共享读委托 / share deny（仍随故障消亡，由 reclaim 语义兜底）；DRC 与
  会话槽缓存跨网关复制（接受重启级别的重放语义，见 §9.6）。
- 多活 + 计划内迁移是本册原语之上的演进，见 §9.9 与 10 册。

## 9.3 集群身份

```toml
[cluster]                      # 全部键与默认值见 08 §8.1
enabled     = true
id          = "3f9c…-uuid"     # 集群标识：所有网关相同
shared_dir  = "/mnt/cephfs/.lightnfs-cluster"   # 共享状态目录（§9.4）
node        = ""               # 本网关名；空 = 主机名
mode        = "failover"       # 本册；active-active 见 10 册
role        = "auto"           # active | standby | auto；standby 不自动接管
fence_lease = "3s"             # 围栏续租周期；围栏 ttl = 3 × 该值
takeover    = "auto"           # auto | manual（manual 只认 `lightnfs-ctl cluster takeover`）
takeover_hook = ""             # 可选可执行脚本，在后端接管钩子之后运行（§9.7）
```

- `server_owner.major_id` 与 `server_scope` 从 `[cluster] id` 派生
  （`server/protocol_stack.cpp` 的 `derive_server_identity`；集群关闭时仍由主机名 +
  `state_dir` 派生）。同一 owner / scope 让客户端把所有网关视为**同一台服务器**；
  `minor_id` 保持 0。这也是 RFC 8881 §2.10.4 trunking 判定的输入——客户端可能对 VIP 之外
  的网关地址尝试 session trunking，因此 VIP 之外的地址不应对外暴露（或 `bind` 只绑 VIP）。
  集群模式下 `server_owner` / `server_scope` 不允许显式配置。
- 句柄 HMAC 密钥集群共享：`shared_dir/hmac.key`（首个网关以 0600 创建，其余只读取）；
  本机 `state_dir/hmac.key` 在集群模式下不再使用。
- 配置一致性：每个网关启动时把导出表的规范化摘要写入 `shared_dir/exports.<node>`，与
  其他网关的摘要逐一比对，不一致则拒绝进入集群（避免 fsid 相同而树不同）。摘要覆盖每个
  导出的 path / fsid / backend / readonly / squash / anon_* 与后端子表键值（按 fsid 排序），
  **按节点豁免键**（`conf` / `keyring` / `id` / `user` / `name` / `log_file` / `fd_cache` /
  `mon_host`）与 `clients` / QoS 不参与——它们是本机凭据或可热重载的策略，不是树身份。
  已永久下线节点的 `exports.<node>` 需运维手动删除，否则新节点会与一份过时摘要比对而被拒。
  - **清单模式下改口径**（`[cluster] exports_source = "catalog"`，[11 册](11-shared-export-catalog.md)
    §11.4）：导出来自共享的 `catalog.toml`，"各网关一致"由单一事实来源天然保证，而各网关是
    **各自在自己的下一个 tick 跟进新版**的，滚动应用期间版本短暂不同属正常。因此此时
    **不再据摘要拒绝启动**——版本差异只记录并告警，由 `catalog.<node>`（每台已应用的版本 +
    状态）与 `lightnfs-ctl cluster catalog status` 暴露给运维。`exports.<node>` 摘要**照写**
    （值 = 合并本机 `[backend_defaults]` 之后导出表的规范摘要，与本地模式同一算法），这样
    本地模式与清单模式混跑的迁移过渡期（11 §11.9）里，本地模式的网关仍受本条校验保护。

## 9.4 共享状态目录

只放 07 §7.5 已定义的**最小稳定状态**，形态不变，位置从本机换成共享目录
（`server/cluster_store.hpp` 顶部注释是键空间的权威）：

```
shared_dir/
  hmac.key                 # 句柄密钥（§9.3）
  epoch                    # 集群 boot epoch：每次接管 +1（§9.5）
  clients/<fnv64>          # reclaim 名单：co_ownerid 原文，由活动网关维护
                           #   （CREATE_SESSION 确认后写、状态全清后删）
  fence                    # 围栏租约："<epoch> <expires_at_unix_ms> <node>\n"，活动网关周期刷新
  exports.<node>           # 导出表摘要（§9.3）
  epoch.lock / fence.lock  # 两个多写者文件的 O_EXCL 串行化；陈旧锁按 2 × fence_lease 回收
  catalog.toml / catalog.history/ / catalog.lock / catalog.<node>
                           # 仅 exports_source = "catalog"：共享导出清单、历史、写锁、每网关已应用版本
                           #   （11 册 §11.3）
```

多活形态在这些文件**旁边**再加一套 per-node / per-fsid 的键（`epoch.<node>`、`nodes/<node>`、
`fence.<node>`、`fs/<fsid>/*`），本册的文件原样不动——键空间全貌见 10 §10.3。

访问方式是 **POSIX 路径**：各集群后端都有本机挂载（ceph 内核客户端 / ceph-fuse、gluster
FUSE、Lustre 客户端），也可以是任何共享文件系统甚至一个小 NFS 目录。写入规则：临时文件 +
`fsync` + `rename` 原子替换（`core::atomic_write_file`），读者永不看到撕裂的记录；`fence`
与 `epoch` 这两个多写者文件的更新用 `O_EXCL` 锁文件串行化，锁文件超过 2 × `fence_lease`
视为写者已死并回收。围栏到期判定留 `kFenceSkewTolerance`（500ms）的时钟容差——**所有网关
必须同步时钟**。

接口是 `server::ClusterStore`（`load_or_create_key` / `read_epoch` / `bump_epoch` /
`list_clients` / `put_client` / `erase_client` / `read_fence` / `acquire_fence` /
`renew_fence` / `release_fence` / `put_exports_digest` / `list_exports_digests`，多活与清单
各自再加一组）；POSIX 目录实现由 `make_posix_cluster_store` 提供，测试用
`tests/mem_cluster_store.hpp` 的内存实现。换一种落地方式（例如经后端 API 写进导出树内的
保留目录、免去额外挂载）只需换这个接口的实现。**每个调用都阻塞在文件系统 IO 上**：只在主
线程或 offload / 控制器自己的线程上调用，绝不在 reactor 上。

延迟预算：reclaim 名单写在 CREATE_SESSION 路径上，共享目录一次 `fsync + rename` 在集群
文件系统上是毫秒级；沿用既有的"写失败只告警、不拒绝会话"策略。

## 9.5 全局 epoch 与围栏

- **epoch 全局单调**：`shared_dir/epoch` 在集群模式下取代本机 `state_dir/boot_epoch`
  （`core/boot_epoch.cpp`）。网关**接管时**（不是进程启动时）`epoch++` 并持久化；写验证器、
  clientid 高 32 位、stateid.other 前 4 字节都取自它，`StateMgr` 既有的 epoch 判定逻辑不变。
  效果：故障网关发出的所有 clientid / stateid 在新网关上零查表即 STALE，未 COMMIT 的
  UNSTABLE 写因验证器变化被客户端重发。
- **围栏**：活动网关每 `fence_lease` 刷新 `fence`，租约 ttl = 3 × `fence_lease`；接管者必须
  先把 `fence` 改写为自己的 `{node, epoch+1}`——`acquire_fence` 只在"无记录 / 已过期（含时钟
  容差）/ 本来就是自己的 / `--force`"时改写，否则回 EBUSY 并原样留下当前持有者。
- **围栏丢失即自我退出**：取得围栏后旧网关若还活着，它的下一次 `renew_fence` 会得到 EPERM
  （记录已不是自己的）→ 立即 `Draining` 并停止服务。共享目录读不到（连续 3 次续租失败）
  同样进 `Draining`。这是 VIP 唯一性之外的第二道保险；两道都失效（脑裂）时两网关 epoch
  不同，客户端会在两个"重启"之间来回 reclaim，行为可诊断（`lightnfs_cluster_epoch` 与
  `lightnfs_cluster_fence_lost_total` 指标），数据不会静默错。

## 9.6 接管流程

网关角色状态机（`server/cluster_controller.{hpp,cpp}` 的 `ClusterController`）：

```
Standby ──(围栏无主/过期 / ctl takeover)──▶ Activating ──▶ Active ──(围栏丢失 / ctl standby / 退出)──▶ Draining ──▶ Standby
```

控制器只拥有"角色 + 围栏"：**何时切换**由它在自己的定时线程上决定，围栏 IO（共享文件系统上
的阻塞调用）也在那条线程上；**数据面动作**（`activate` / `deactivate`、后端接管钩子）一律
投递到主循环执行，且投递期间围栏继续续租——慢接管不会让租约在自己脚下过期。每次转换由一把
互斥锁串行化。

- **Standby**：进程已启动、配置已校验、后端已 `start()`（集群连接已建立），**不建协议栈、
  不监听**。因为 `ProtocolStack` 在构造时固化 epoch（写验证器、`StateMgr` 配置、伪根），
  接管时重建它是最简单也最安全的做法（无状态可丢）。管理面（ctl / metrics）与数据面分离，
  standby 期间照常可用。定时线程轮询 `fence`；`role = "standby"` 或 `takeover = "manual"`
  时只等 `lightnfs-ctl cluster takeover`。
- **Activating**（目标 < 1s，`lightnfs_cluster_activation_seconds` 直方图覆盖）：
  1. `acquire_fence`（拿不到即回 Standby），随后 `bump_epoch`——从这一刻起故障网关发出的
     每个 clientid / stateid 都是 STALE；
  2. **后端接管钩子**（§9.7）：每个导出的 `Backend::takeover()` 在 reactor 0 上跑（与
     `start()` / `stop()` 同一线程纪律），再跑运维的 `takeover_hook` 脚本。钩子失败只告警，
     接管继续——§9.7 第 1 条的 DELAY 重试覆盖仍被别人持着的锁；
  3. 构造 `ProtocolStack`（固化新 epoch），`StateMgr` 从 `shared_dir/clients/` 载入 reclaim
     名单并进入 grace（时长 `[protocol] grace`，见 §9.8）；
  4. `Frontend::start()` 开始监听；VIP 漂移由外部 HA 与此并行完成。
     任一步失败 → 释放围栏、回 Standby、`lightnfs_cluster_activation_failures_total` +1。
- **Active**：与单机形态完全相同，只多了围栏续租与 reclaim 名单写共享目录。
- **Draining**：停 accept → 等存量连接自然收敛（超时后强制关闭）→ 拆掉协议栈 → 重建后端
  连接 → 释放围栏（围栏是被别人抢走的则**不**释放，避免删掉接手者的记录）→ 回到 Standby。

客户端侧时间线（v4.1，Linux 客户端行为，RFC 8881 §8.4.2.1）：VIP 漂移 → TCP 重连到新网关
→ SEQUENCE 带旧 sessionid → NFS4ERR_BADSESSION → EXCHANGE_ID（同 co_ownerid；server_owner
未变，客户端判定"服务器重启"）→ 新 clientid（新 epoch）→ CREATE_SESSION（此时名单已含它）
→ 对每个打开文件 OPEN(CLAIM_PREVIOUS)、每把锁 LOCK(reclaim=true)、RECLAIM_COMPLETE →
继续 IO。期间新建状态类操作回 GRACE 由客户端重试，读放行（07 §7.5 既有策略）。
全部名单内客户端 RECLAIM_COMPLETE 后提前出 grace。

v3 客户端：无状态，句柄同密钥可验，只感知一次 TCP 重连 + 写验证器变化（重发未提交写）。
MOUNT 无需处理（UMNT 本就是空实现）。

**接受的重放边界**（与 knfsd 重启相同）：切换瞬间在途的非幂等请求（v3 经 DRC、v4 经槽
缓存）在新网关上没有缓存副本，客户端重传后会再执行一次。

## 9.7 后端接管钩子

故障网关进程死亡后，它在存储侧持有的锁 / 会话不会立刻消失；新网关在 grace 内 reclaim
LOCK 时下推被拒（EAGAIN → DENIED）会让客户端**丢锁**。两处配合解决：

1. **状态层**：reclaim 模式（`LockArgs.reclaim`）下的下推失败回 **NFS4ERR_DELAY** 并在
   grace 内重试，不回 DENIED；grace 结束仍失败才 DENIED（此时确实有别人持锁）。计数
   `native_lock_reclaim_delays`。
2. **后端接口**的可选钩子 `Backend::takeover(const ClusterIdentity&)`（默认空操作），在
   Activating 第 2 步调用，尽快让存储侧释放故障网关的残留：
   - **CephFS**：libcephfs 为此提供了成套原语——`ceph_set_uuid(uuid)` +
     `ceph_start_reclaim(uuid, CEPH_RECLAIM_RESET)` / `ceph_finish_reclaim()`：以同一 uuid
     接管旧会话，MDS 立即驱逐旧会话并释放其 caps / 锁（Ganesha HA 同法）。会话 uuid 取
     `[export.cephfs] uuid`，默认 `<cluster id>-<fsid>`，各网关相同；standby 的会话不带
     uuid。MDS 不支持（EOPNOTSUPP）或 libcephfs 太旧（缺 `ceph_start_reclaim`）只告警，
     残留按 MDS 自身的会话超时清理，此时 grace 内的 LOCK reclaim 走第 1 条的 DELAY 重试。
   - **GlusterFS**：锁随连接释放；旧网关的 TCP 断开后砖块按 `network.ping-timeout`
     （默认 42s）清理。钩子无事可做，部署要求把 ping-timeout 调到 ≤ grace/2；正常 drain
     时旧网关自己先 `glfs_fini`。
   - **Lustre**：OFD 锁随客户端驱逐释放（`obd_timeout`）；驱逐动作（`lctl set_param
     mdc.*.evict_client=<old nid>`）需特权，作为 `takeover_hook` 外部脚本而非内置。

   钩子之后跑 `[cluster] takeover_hook`（可执行文件，超时 `fence_lease`，环境变量
   `LNFS_CLUSTER_ID` / `LNFS_NODE` / `LNFS_EPOCH` / `LNFS_PREV_NODE`）。`LNFS_PREV_NODE`
   是被替换的围栏记录所属的节点——首次启动或替换自己上一次的记录时为空。
3. **委托**：故障网关授予的读委托随之消亡。客户端用 CLAIM_DELEG_PREV_FH 试图找回，网关把
   它**接受为普通 open 状态、不再授予委托**，与 CLAIM_PREVIOUS 同一门禁（名单 + grace），
   让这条路径零退避；grace 期间不授予新委托。

## 9.8 参数取值

- `grace`：必须覆盖"客户端察觉故障 + 重连 + 重新建会话"的时间。Linux 客户端在 TCP
  重连后立即重试，VIP 漂移通常 1–5s；建议 `grace = "30s"`（默认 `auto` = lease 90s，
  不必等于它），名单内客户端全部 RECLAIM_COMPLETE 会提前结束。
- `lease`：接管期间旧租约无意义（epoch 已变）；保持 90s。courtesy 机制在新网关上从零开始。
- `fence_lease`：3s，失效判定 3×；过短在共享目录抖动时误接管，过长拖慢自动接管。
- 后端超时：CephFS `client_session_timeout`（配合 reclaim 原语可不改）；Gluster
  `network.ping-timeout ≤ grace/2`；Lustre `obd_timeout` 同理。

## 9.9 演进：多活与计划内迁移

主备只用了一台网关的算力。**多活**把本册的原语（`ClusterStore`、围栏租约、集群身份、后端
接管钩子、per-fsid 会话 uuid）从"每进程一个角色"推广到"**每个导出一个属主网关**"：围栏、
epoch、grace、reclaim 名单都按 fsid（`shared_dir/fs/<fsid>/…` 与按节点批量的
`fence.<node>`），不同导出分布在不同网关；伪根在所有网关一致，跨进一个导出的边界时非属主
网关回 **NFS4ERR_MOVED** 并在 `fs_locations` 属性（RFC 8881 §11）里给出属主地址，客户端按
§11.10 对该文件系统重新 EXCHANGE_ID / CREATE_SESSION 并 reclaim（"迁移 = 该 fs 的服务器
重启"，不需要实现 §11.10.1 的透明状态搬运）。计划内迁移（`cluster migrate`）是同一机制的
主动形态：源网关对该 fsid 进入 Draining、写 owner 记录后放围栏，目标网关下一次轮询接管。

完整方案、协议面改动（`fs_locations` / `fs_locations_info`、MOVED、`SEQ4_STATUS_LEASE_MOVED`）、
per-fsid 键空间与代码锚点见 [10 册](10-multi-gateway-active-active.md)；`[cluster] mode =
"failover"`（本册）与 `"active-active"`（10 册）是互斥取值。

## 9.10 代码地图

| 位置 | 职责 |
|------|------|
| `core/config.{hpp,cpp}` | `[cluster]` 段解析与校验（`enabled` 时 `shared_dir` 须为绝对路径、`role` / `takeover` 取值、`takeover_hook` 须可执行、`server_owner` / `server_scope` 不得显式设置）；后端构造后查每导出 `kStableHandles + kByteLocks + native_locks`（`unsafe_skip_backend_checks` 仅测试降级为告警） |
| `server/cluster_store.{hpp,cpp}` | 共享目录访问：密钥 / epoch / reclaim 名单 / 围栏 / 导出摘要（多活与清单的键在同一接口上扩展）；`atomic_write_file` 原子写 + `O_EXCL` 锁文件 |
| `state/state_mgr.cpp`、`core/file_handle.cpp` | 集群模式下句柄密钥、epoch、reclaim 名单三处改走 `ClusterStore`，本机实现保持原语义；reclaim 下推失败 → grace 内 DELAY 重试（`native_lock_reclaim_delays`） |
| `server/data_plane.{hpp,cpp}`、`server/frontend.cpp` | 管理面（ctl / metrics）与数据面分离，不随 `Frontend` 生死；`ProtocolStack` 可重建 + 连接收敛（drain → close_all → wait_idle） |
| `server/protocol_stack.cpp` | `server_owner` / `server_scope` 从 `[cluster] id` 派生（`derive_server_identity`） |
| `nfsv4/engine.cpp` | CLAIM_DELEG_PREV_FH 接受为普通 open 状态（名单 + grace 门禁） |
| `server/daemon.cpp` | 启动期写 / 比对 `exports.<node>` 摘要；主循环承载 activate / deactivate 与接管钩子；SIGHUP 热重载 |
| `server/cluster_controller.{hpp,cpp}` | `ClusterController` 状态机 Standby / Activating / Active / Draining + 定时线程围栏续租；集群指标提供者（随进程存活，standby 期间也可见） |
| `server/takeover_hook.{hpp,cpp}` | `[cluster] takeover_hook` 外部脚本执行（超时、环境变量） |
| `backend/api.{hpp,cpp}` | 可选 `Backend::takeover(ClusterIdentity)`，默认空操作 |
| `backend/cephfs/*` | `cephapi` 的 `ceph_set_uuid` / `ceph_start_reclaim` / `ceph_finish_reclaim`；`CephBackend::takeover()`；`[export.cephfs] uuid` |
| `server/ctl.cpp`、`tools/lightnfs_ctl.cpp` | `cluster status` / `cluster takeover [--force]` / `cluster standby`（多活下同一组命令按导出生效，10 册） |

**指标**（08 §8.3）：`lightnfs_cluster_role{role}`（one-hot）、`_epoch`、`_fence_owned`、
`_fence_age_seconds`、`_takeovers_total`、`_fence_lost_total`、`_activation_failures_total`、
`_activation_seconds` 直方图。由控制器注册、随进程存活，standby 期间也可见；多活形态下这一
整机系列不出样本（改出 per-fsid 系列，10 §10.13）。

## 9.11 验证

- **单机双实例（无 root）**：`scripts/accept_failover_local.sh` + `lnfs_accept_client`
  的 `v4failover` 模式——两个 lightnfsd 共用一个本地 `shared_dir`，监听不同端口模拟 VIP
  切换；客户端连 A → OPEN + LOCK + UNSTABLE 写 → `kill -9 A` → `ctl cluster takeover` B →
  改连 B 端口 → 断言 BADSESSION → EXCHANGE_ID / CREATE_SESSION → CLAIM_PREVIOUS 与 LOCK
  reclaim 全部成功 → COMMIT 因验证器变化重发 → 数据逐字节一致；同一轮跑 v3 `wtest` 证明
  v3 侧无感。脚本另有脑裂段（围栏被改写后旧网关自我 drain）。Release 与 ASAN 各一轮。
- **后端 fake 的残留锁演练**：`tests/reclaim_probe.hpp` 向 `cephapi_fake` / `gfapi_fake`
  注入"旧会话残留锁"（以及"锁在 N 秒后释放"），`tests/test_{cephfs,gluster,lustre}.cpp`
  据此覆盖 §9.7 第 1 条的 DELAY-重试路径与接管钩子。
- **单元**：`tests/test_cluster_store.cpp`（原子写、锁回收、围栏 CAS 语义）、
  `tests/test_cluster_controller.cpp`（状态机、围栏被改写后的自我 drain、续租失败计数）、
  `tests/test_ctl.cpp`（`cluster` 子命令的文本与 `--json`）；共享目录用
  `tests/mem_cluster_store.hpp`。
- **root VM**（人工，需 root）：`scripts/accept_failover_vm.sh`——keepalived VIP + 两台网关
  + 内核客户端跑 fsx / cthon lock 组，切换中 `kill -9` 活动网关，负载不中断、无 EIO /
  ESTALE。本机环境无 root，执行情况见
  [../toto/multi-gateway-failover-followups.md](../toto/multi-gateway-failover-followups.md)。
