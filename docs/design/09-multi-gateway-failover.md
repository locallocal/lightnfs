# 9. 多网关无感故障切换（共享后端）——设计方案

> 状态：**已实现（2026-09-05）**。原按阶段拆分的实施计划（A 基础设施 / B 协议语义 /
> C 进程生命周期 / D 后端接管钩子 / E 验证与文档）全部完成并合入；那份逐步文档在完成后
> 撤下，每步的详细实现记录保留在 git 历史里。当前实现总览见本册 §9.10 改动清单（带代码
> 位置）；**尚未闭环的取舍、待决与发布前必补的门槛**汇总在
> [docs/toto/multi-gateway-failover-followups.md](../toto/multi-gateway-failover-followups.md)。本册记
> "做什么、为什么"。目标是在多个 lightnfsd 网关共挂同一个共享后端（GlusterFS / Lustre /
> CephFS，或任何满足 §9.2 前提的后端）时，一个网关故障后客户端切到另一个网关
> **不重挂载、不重建应用状态**：打开的文件、字节锁、未提交的写都由协议机制恢复。

## 9.1 现状与差距

现在的多网关形态（05 §5.8、06、07 §7.6）只解决了"同时服务"的一半：

| 已具备 | 缺失（本方案要补的） |
|--------|----------------------|
| 集群后端的句柄跨网关稳定（GFID / FID / vinodeno，`kStableHandles`） | 句柄 HMAC 密钥 `state_dir/hmac.key` 每网关各自生成 → 另一网关回 BADHANDLE |
| 字节锁下推到存储（`kByteLocks`），网关之间互斥 | v4 状态（clientid/会话/open/lock/委托）只在网关内存，reclaim 名单只在本机 `state_dir/clients/` |
| CephFS 有原生 change（跨网关 CTO） | `server_owner`/`server_scope` 按主机名 + state_dir 派生 → 客户端把两台网关当**两台服务器** |
| 重启后 grace + CLAIM_PREVIOUS 的完整 reclaim 链（07 §7.5） | boot epoch 每网关独立 → 两台网关可能用相同 epoch，旧 stateid 不会被判 STALE |
| 读委托 + 回传通道 | 故障网关的存储侧锁/会话残留到超时前，新网关 reclaim 时下推被拒 |

结论：客户端切换后看到的是"一台陌生的服务器"，只能重建状态。**方案的核心思路是把
"切换到另一台网关"变成客户端早已会处理的"同一台服务器重启"**（RFC 8881 §8.4.2.1）：
统一身份 + 共享 reclaim 名单 + 全局单调 epoch + 接管即进 grace。这与 knfsd 共享
`/var/lib/nfs`、Ganesha 集群 recovery backend 的做法同构，Linux 客户端的恢复路径是现成的。

## 9.2 前提与范围

**前提**（不满足则本方案不适用，配置校验期拒绝）：

1. 所有网关导出**同一棵树**：`[[export]]` 的 `path`/`fsid`/`backend` 与后端子表逐项一致；
2. 后端置位 `kStableHandles`（local 后端仅内核句柄模式满足，且各网关要看到同一挂载——
   本方案面向集群后端，local 只在共享块设备 + 集群文件系统上有意义）；
3. 后端置位 `kByteLocks` 并开启 `native_locks`——否则两网关的锁互不可见，"无感"只剩
   open 状态；
4. 客户端经**单一服务地址**访问（VIP / DNS 单名），地址漂移由外部 HA（keepalived、
   pacemaker、云 LB）完成；lightnfs 不做仲裁选主，只提供"接管"动作与围栏。

**范围**：

- 目标 1（本方案主体）：**主备 + 接管**——同一时刻一个网关对外服务；故障后另一网关接管，
  v3 客户端无感，v4.1 客户端在一个 grace 窗口内 reclaim 全部 open/lock。
- 目标 2（§9.9，后续）：多活 + 计划内迁移（每导出一个活动网关，用 FS_LOCATIONS +
  NFS4ERR_MOVED 把客户端引到目标网关）。
- 非目标：跨网关共享读委托/share deny（仍随故障消亡，由 reclaim 语义兜底）；DRC 与
  会话槽缓存跨网关复制（接受重启级别的重放语义，见 §9.6）。

## 9.3 集群身份

新增配置段：

```toml
[cluster]
enabled    = true
id         = "3f9c…-uuid"      # 集群标识：所有网关相同
shared_dir = "/mnt/cephfs/.lightnfs-cluster"   # 共享状态目录（§9.4）
role       = "auto"            # active | standby | auto（按围栏租约自动接管）
fence_lease = "3s"             # 活动网关续租周期；3× 未续视为失效
takeover   = "auto"            # auto | manual（manual 只接受 `lightnfs-ctl cluster takeover`）
```

- `server_owner.major_id` 与 `server_scope` 从 `[cluster] id` 派生（现在是主机名 +
  state_dir，`server/protocol_stack.cpp`）。同一 owner/scope 让客户端把所有网关视为
  **同一台服务器**；`minor_id` 保持 0。这也是 RFC 8881 §2.10.4 trunking 判定的输入——
  客户端可能对 VIP 之外的网关地址尝试 session trunking，方案要求 VIP 之外的地址不对外暴露
  （或 `bind` 只绑 VIP）。
- 句柄 HMAC 密钥改为集群共享：`shared_dir/hmac.key`（首个网关生成，0600，之后只读取）；
  本机 `state_dir/hmac.key` 在集群模式下不再使用。
- 配置一致性：每个网关启动时把导出表的规范化摘要写入 `shared_dir/exports.<node>`，与
  其他网关的摘要不一致则拒绝进入集群（避免 fsid 相同而树不同）。

## 9.4 共享状态目录

只放 07 §7.5 已定义的**最小稳定状态**，形态不变，位置从本机换成共享目录：

```
shared_dir/
  hmac.key                 # 句柄密钥（§9.3）
  epoch                    # 集群 boot epoch：每次接管 +1（§9.5）
  clients/<hash(co_ownerid)>   # reclaim 名单：由活动网关维护（CREATE_SESSION 确认后写、状态全清后删）
  fence                    # 围栏租约：{node, epoch, expires_at}，活动网关周期刷新
  exports.<node>           # 导出表摘要（§9.3）
```

访问方式：v1 用 **POSIX 路径**（各集群后端都有本机挂载：ceph 内核客户端 / ceph-fuse、
gluster FUSE、Lustre 客户端；也可以是任何共享文件系统甚至一个小 NFS 目录）。写入规则：
临时文件 + `fsync` + `rename` 原子替换；`fence` 与 `epoch` 的更新用 `O_EXCL` 锁文件串行化。
后续可加"经后端 API 写入导出树内保留目录"的实现，免去额外挂载——接口抽成
`server::ClusterStore { load_key, bump_epoch, put/erase_client, acquire/renew/release_fence }`，
两种实现可换。

延迟预算：reclaim 名单写在 CREATE_SESSION 路径上，共享目录一次 `fsync + rename` 在集群
文件系统上是毫秒级；沿用现有"写失败只告警不拒绝会话"的策略（`state_mgr.cpp:237`）。

## 9.5 全局 epoch 与围栏

- **epoch 全局单调**：`shared_dir/epoch` 取代本机 `state_dir/boot_epoch`（`core/boot_epoch.cpp`）。
  网关**接管时**（不是进程启动时）`epoch++` 并持久化；写验证器、clientid 高 32 位、
  stateid.other 前 4 字节都取自它（现有 `StateMgr` 的 epoch 判定 `state_mgr.cpp:103/1011/1050`
  不用改）。效果：故障网关发出的所有 clientid/stateid 在新网关上零查表即 STALE，
  未 COMMIT 的 UNSTABLE 写因验证器变化被客户端重发。
- **围栏**：活动网关每 `fence_lease` 刷新 `fence`；接管者必须先把 `fence` 改写为
  自己的 `{node, epoch+1}`（`O_EXCL` 锁文件 + 检查旧租约已过期或 `takeover=manual` 强制）。
  取得围栏后旧网关若还活着，会在下一次刷新时发现 `fence` 不是自己 → 立即 `drain` 并
  退出服务（`daemon` 的守护协程检查）。这是 VIP 唯一性之外的第二道保险；两道都失效
  （脑裂）时两网关 epoch 不同，客户端会在两个"重启"之间来回 reclaim，行为可诊断
  （`lightnfs_cluster_epoch` 指标），数据不会静默错。

## 9.6 接管流程

网关角色状态机（`server/daemon.cpp` 新增 `ClusterController`）：

```
Standby ──(围栏过期 / ctl takeover)──▶ Activating ──▶ Active ──(围栏丢失 / ctl standby / 退出)──▶ Draining
```

- **Standby**：进程已启动、配置已校验、后端已 `start()`（集群连接已建立），**不建协议栈、
  不监听**。因为 `ProtocolStack` 在构造时固化 epoch（写验证器、`StateMgr` 配置、伪根），
  接管时重建它是最简单也最安全的做法（无状态可丢）。守护协程轮询 `fence`。
- **Activating**（目标 < 1s）：
  1. 取围栏、`epoch++`（§9.5）；
  2. 构造 `ProtocolStack`，`StateMgr::load_grace_list()` 改读 `shared_dir/clients/` → 进入
     grace（时长 `[protocol] grace`，见 §9.8 取值）；
  3. 后端"接管钩子"（§9.7）清理故障网关在存储侧的残留；
  4. `Frontend::start()` 开始监听；VIP 漂移由外部 HA 与此并行完成。
- **Active**：与今天完全相同，只多了围栏续租与 reclaim 名单写共享目录。
- **Draining**：`drain`（停 accept）→ 存量连接自然收敛 → 释放围栏 → 回到 Standby 或退出。

客户端侧时间线（v4.1，Linux 客户端行为，RFC 8881 §8.4.2.1）：VIP 漂移 → TCP 重连到新网关
→ SEQUENCE 带旧 sessionid → NFS4ERR_BADSESSION → EXCHANGE_ID（同 co_ownerid；server_owner
未变，客户端判定"服务器重启"）→ 新 clientid（新 epoch）→ CREATE_SESSION（此时名单已含它）
→ 对每个打开文件 OPEN(CLAIM_PREVIOUS)、每把锁 LOCK(reclaim=true)、RECLAIM_COMPLETE →
继续 IO。期间新建状态类操作回 GRACE 由客户端重试，读放行（07 §7.5 既有策略）。
全部名单内客户端 RECLAIM_COMPLETE 后提前出 grace。

v3 客户端：无状态，句柄同密钥可验，只感知一次 TCP 重连 + 写验证器变化（重发未提交写）。
MOUNT 无需处理（UMNT 本就是空实现）。

**接受的重放边界**（与 knfsd 重启相同）：切换瞬间在途的非幂等请求（v3 经 DRC、v4 经槽
缓存）在新网关上没有缓存副本，客户端重传后会再执行一次；文档明示。

## 9.7 后端接管钩子

故障网关进程死亡后，它在存储侧持有的锁/会话不会立刻消失；新网关在 grace 内 reclaim
LOCK 时下推被拒（EAGAIN → 现在映射为 DENIED，`state_mgr.cpp:1800`）会让客户端**丢锁**。
两处配合解决：

1. **状态层**：reclaim 模式（`LockArgs.reclaim`）下的下推失败改回 **NFS4ERR_DELAY**
   并在 grace 内重试，不回 DENIED；grace 结束仍失败才 DENIED（此时确实有别人持锁）。
2. **后端接口**新增可选钩子 `Backend::takeover(const ClusterIdentity&)`（默认空操作），
   在 Activating 第 3 步调用，尽快让存储侧释放故障网关的残留：
   - **CephFS**：libcephfs 为此提供了成套原语——`ceph_set_uuid(role_uuid)` +
     `ceph_start_reclaim(old_uuid, CEPH_RECLAIM_RESET)` / `ceph_finish_reclaim()`：以同一
     uuid 接管旧会话，MDS 立即驱逐旧会话并释放其 caps/锁（Ganesha HA 同法）。配置
     `[export.cephfs] uuid` 取 `[cluster] id + fsid`，各网关相同；`cephapi.hpp` 函数表
     加这 3 个入口。
   - **GlusterFS**：锁随连接释放；旧网关的 TCP 断开后砖块按 `network.ping-timeout`
     （默认 42s）清理。钩子无事可做，文档要求把 ping-timeout 调到 ≤ grace/2，或在
     围栏丢失路径上让旧网关先 `glfs_fini`（正常 drain 时已如此）。
   - **Lustre**：OFD 锁随客户端驱逐释放（`obd_timeout`）；钩子可选执行
     `lctl set_param mdc.*.evict_client=<old nid>`（需特权，作为可配脚本钩子而非内置）。
3. 委托：故障网关授予的读委托随之消亡。客户端会用 CLAIM_DELEG_PREV_FH 试图找回，现在回
   NOTSUPP（`engine.cpp:1531`）→ Linux 客户端归还委托并按普通 OPEN 重开，在 grace 内会
   收到 GRACE 而反复重试。方案把 CLAIM_DELEG_PREV_FH 实现为"接受为普通 open 状态、
   不再授予委托"，与 CLAIM_PREVIOUS 同一门禁（名单 + grace），让这条路径零退避。
   grace 期间不授予新委托。

## 9.8 参数取值

- `grace`：必须覆盖"客户端察觉故障 + 重连 + 重新建会话"的时间。Linux 客户端在 TCP
  重连后立即重试，VIP 漂移通常 1–5s；建议 `grace = "30s"`（不必等于 lease 90s），
  名单内客户端全部 RECLAIM_COMPLETE 会提前结束。
- `lease`：接管期间旧租约无意义（epoch 已变）；保持 90s。courtesy 机制在新网关上从零开始。
- `fence_lease`：3s，失效判定 3×；过短在共享目录抖动时误接管，过长拖慢自动接管。
- 后端超时：CephFS `client_session_timeout`（配合 reclaim 原语可不改）；Gluster
  `network.ping-timeout ≤ grace/2`；Lustre `obd_timeout` 同理。

## 9.9 演进：多活与计划内迁移

主备只用了一台网关的算力。多活形态：**每个导出一个活动网关**（围栏与 epoch 按导出而非
按网关，`shared_dir/<fsid>/…`），不同导出分布在不同网关；伪根在所有网关一致。计划内迁移
一个导出：源网关对该 fsid 进入 Draining 并对新请求回 **NFS4ERR_MOVED**，`fs_locations`
属性（RFC 8881 §11）指向目标网关地址；目标网关按 §9.6 接管该导出。客户端按 RFC 8881
§11.10 处理 MOVED：对该文件系统重新 EXCHANGE_ID/CREATE_SESSION 到目标并 reclaim
（"迁移 = 该 fs 的服务器重启"，无需实现透明状态迁移 §11.10.1 的状态搬运）。本阶段新增
的协议面：`fs_locations`/`fs_locations_info` 属性、MOVED 状态码、`SEQ4_STATUS_LEASE_MOVED`。

## 9.10 改动清单（已实现）

| 位置 | 改动 | 阶段 |
|------|------|------|
| `core/config.{hpp,cpp}` | `[cluster]` 段解析与校验（enabled 时要求 shared_dir 绝对路径、后端 kStableHandles+kByteLocks+native_locks；`unsafe_skip_backend_checks` 仅测试放宽） | A1 |
| `server/cluster_store.{hpp,cpp}`（新） | 共享目录访问：key / epoch / clients / fence / exports 摘要，`atomic_write_file` 原子写 | A2 |
| `state/state_mgr.cpp`、`core/file_handle.cpp` | 集群模式下句柄密钥 / epoch / reclaim 名单三处改走 `ClusterStore` 接口，本机实现保持原语义 | A3 |
| `server/data_plane.{hpp,cpp}`、`server/frontend.cpp` | 管理面（ctl / metrics）与数据面分离，不随 `Frontend` 生死 | A4 |
| `server/protocol_stack.cpp` | server_owner/scope 从 `[cluster] id` 派生（`derive_server_identity`） | B1 |
| `state/state_mgr.cpp` | reclaim 模式下推失败 → DELAY 在 grace 内重试（`native_lock_reclaim_delays`） | B2 |
| `nfsv4/engine.cpp` | CLAIM_DELEG_PREV_FH → 普通 open 状态（名单 + grace 门禁） | B3 |
| `server/daemon.cpp`、`server/cluster_store.cpp` | 启动期导出表一致性摘要 `exports.<node>`，树不一致拒绝入集群 | B4 |
| `server/data_plane.cpp`、`server/frontend.cpp` | `ProtocolStack` 可重建 + 连接收敛等待（drain → close_all → wait_idle） | C1 |
| `server/cluster_controller.{hpp,cpp}`（新）、`server/daemon.cpp` | `ClusterController` 状态机 Standby/Activating/Active/Draining + 定时线程围栏续租；`activate/deactivate` 投递到主循环 | C2 |
| `server/ctl.cpp`、`tools/lightnfs_ctl.cpp` | `cluster status` / `cluster takeover [--force]` / `cluster standby` | C3 |
| `server/cluster_controller.cpp`（`obs`） | `lightnfs_cluster_role`/`_epoch`/`_fence_owned`/`_fence_age_seconds`/`_takeovers_total`/`_fence_lost_total`/`_activation_failures_total`/`_activation_seconds` | C4 |
| `backend/api.{hpp,cpp}`、`server/takeover_hook.{hpp,cpp}`（新）、`server/daemon.cpp` | 可选 `Backend::takeover(ClusterIdentity)`（默认空操作）+ `[cluster] takeover_hook` 外部脚本 | D1 |
| `backend/cephfs/*` | cephapi 增 `ceph_set_uuid/ceph_start_reclaim/ceph_finish_reclaim`；`CephBackend::takeover()`；`[export.cephfs] uuid` | D2 |
| `tests/accept_client.cpp`、`scripts/accept_failover_local.sh`（新） | `v4failover` 验收模式 + 本机双实例脚本（Release + ASAN） | E1 |
| `tests/reclaim_probe.hpp`（新）、`tests/{cephapi,gfapi}_fake`、`tests/test_{cephfs,gluster,lustre}.cpp`、`scripts/accept_failover_vm.sh`（新） | 残留锁注入 + B2 端到端演练（三后端）；脑裂演练 | E2 |
| 文档 | 08 册配置、deployment.md §5 多网关主备（VIP + keepalived）、07 §7.5 稳定存储位置、05 册 `takeover()`、本册状态、design/README 目录 | E3 |


## 9.11 验证方案（已落地）

实现映射：单机双实例 = `scripts/accept_failover_local.sh` + `lnfs_accept_client v4failover`（E1）；
后端 fake 残留锁 + B2 演练 = `tests/test_{cephfs,gluster,lustre}.cpp` 经 `tests/reclaim_probe.hpp`（E2）；
脑裂 = `test_cluster_controller`（围栏改写）+ E1 脚本的脑裂段；root VM = `scripts/accept_failover_vm.sh`（人工）。

- **单机双实例（无 root）**：两个 lightnfsd 共用一个本地 `shared_dir`（tmpfs），监听不同
  端口模拟 VIP 切换；`lnfs_accept_client` 新增 `v4failover` 模式：连 A → OPEN + LOCK +
  UNSTABLE 写 → `kill -9 A` → `ctl cluster takeover` B → 客户端改连 B 端口 → 断言
  BADSESSION → EXCHANGE_ID/CREATE_SESSION → CLAIM_PREVIOUS/LOCK reclaim 全部成功 →
  COMMIT 因验证器变化重发 → 数据逐字节一致；同时跑 v3 `wtest` 证明无感。
- **后端 fake**：`cephapi_fake` 增 uuid/reclaim 原语与"旧会话残留锁"注入，覆盖 §9.7
  的 DELAY-重试路径；gluster/lustre fake 注入"锁在 N 秒后释放"。
- **root VM**：keepalived VIP + 两台网关 + 内核客户端跑 fsx/cthon lock 组，切换中
  `kill -9` 活动网关，负载不中断、无 EIO/ESTALE；测三种后端各一轮。
- **脑裂演练**：人为让围栏与 VIP 判定分离，验证旧网关在下一次续租时自我 drain。
