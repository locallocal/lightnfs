# 11. 共享导出清单（集群级集中式导出配置）——设计与实现

> `[cluster] exports_source = "catalog"`：导出表从每台网关的本地 TOML 上移到
> `shared_dir/catalog.toml`——集群一份、带版本号与历史、只由管理命令写；本地只留身份与
> `[backend_defaults]` 本机键。于是增删导出、改属主顺位 `nodes`、改 `clients` / QoS /
> `readonly` / `squash` / `anon_*` 都不再需要改 N 份文件、重启 N 台网关。默认值
> `exports_source = "local"` 下导出仍来自本地 `[[export]]`，行为一如不启用清单时。运维视角见
> [../guide/deployment.md](../guide/deployment.md) §6.1，配置 / 热重载口径 / 指标 / ctl 见
> [08 册](08-config-observability.md)，未闭环项见
> [followups/shared-export-catalog-followups.md](followups/shared-export-catalog-followups.md)。
>
> 本册长在 [09 册](09-multi-gateway-failover.md) 主备与 [10 册](10-multi-gateway-active-active.md)
> 多活之上：`ClusterStore`（09 §9.4）、导出表摘要一致性校验（09 §9.3）、`FsClusterController`
> 与 per-fsid 围栏 / 属主视图（10 §10.3）、`cluster migrate`（10 §10.7）、热重载子集
> （08 §8.1，`ExportTable::reload_dynamic`）都来自那两册；本册只加"导出配置的来源与生命周期"。
> 主备与多活都可用清单。

## 11.1 问题与目标

**本地模式（`exports_source = "local"`，默认）的局限**（10 §10.10）：集群里每台网关各带一份本地
TOML，其中 `[[export]]` 段（path / fsid / backend / squash / readonly / anon / 后端子表 / `nodes`）
**在全集群必须逐字相同**——启动时用 `exports.<node>` 摘要互相校验，不一致的网关拒绝入集群。于是：

- **加一个导出 = 改 N 份文件 + 重启 N 台网关**：导出集启动后不可增删（`ExportTable::reload_dynamic`
  只接受 clients 白名单与 QoS，其余一律"restart required"）。
- **改 `nodes`（属主顺位）也要重启**：`nodes` 进导出摘要且被 `reload_dynamic` 判为拓扑变化。
- **没有单一事实来源**：谁是"正确的导出表"由"最先起来的那台"决定，改错一台就是一次入集群失败。

**目标**：

1. **集群级导出清单（catalog）放在共享存储**：`shared_dir/catalog.toml` 是导出表的**唯一事实
   来源**；网关本地 TOML 只保留本机身份与本机相关的键。
2. **管理命令改清单**：`lightnfs-ctl cluster catalog …` / `cluster export add|set|remove …` 对任一网关
   发出，校验后写入共享存储（带版本号与历史，可回滚）。
3. **网关自动或手动跟进**：`[cluster] catalog_refresh = auto`（默认）时网关在围栏 tick 里发现新
   版本即应用；`manual` 时只记"有新版本待应用"，由 `lightnfs-ctl cluster catalog apply` / `reload` /
   SIGHUP 触发。
4. **不重启地增删导出、改属主顺位**：新导出由其 `nodes[0]` 按 10 §10.3 的规则接管；`nodes` 变更
   使不在新名单里的属主平滑迁出；删除的导出在属主上 drain。
5. **零默认行为变化**：`exports_source = "local"`（默认）下一切不变——清单带来的每一处行为差异
   都挂在 `exports_source = "catalog"` 之后；failover 与 active-active 都可用清单。

**非目标**：

- 不做配置中心 / 服务发现：清单仍只经 `ClusterStore` 走共享目录，**不加网关间 RPC**（沿用 09/10
  "lightnfs 不选主、不通信"的原则）。
- 不集中 `[server]`、`[cluster]` 身份、日志、TLS、资源限制等**本机**配置——那些本来就该按机器配。
- 不在线改一个 fsid 的 `path` / `backend` / 后端标识键（句柄编码绑定 fsid + 后端 ObjId，见 §11.8）。
- 不做多写者并发合并：清单写入是"读-改-写 + 版本 CAS"，并发写入方之一失败重试。

## 11.2 什么进清单、什么留本地

| 键 | 本地模式 | 清单模式 | 理由 |
|----|----------|----------|------|
| `[[export]] path / fsid / backend / readonly / squash / anon_uid / anon_gid` | 本地，进摘要 | **清单** | 导出身份，全集群必须一致 |
| `[[export]] nodes` | 本地，进摘要，改动需重启 | **清单**，在线生效 | 属主顺位是集群决策 |
| `[[export]] clients / read_bps / write_bps / iops` | 本地，热重载 | **清单**，在线生效 | 客户端从任一网关看到的策略应一致 |
| `[[export]] disabled` | 不接受（EINVAL） | **清单** | 保留 fsid 但下线（维护 / 删除前置） |
| `[export.<backend>]` 集群键（volume / fs_name / subdir / mount …） | 本地，进摘要 | **清单** | 与导出身份同级 |
| `[export.<backend>]` 本机键 `kPerNodeBackendKeys`（conf / keyring / id / user / name / log_file / fd_cache / mon_host） | 本地，不进摘要 | **本地** `[backend_defaults.<backend>]` | 凭据、日志路径、缓存大小按机器配 |
| `[server]`、`[cluster]`、`[log]`、`[protocol]` 等 | 本地 | 本地 | 本机运行参数 |

**合并规则**（`core/catalog.hpp` 的 `merge_with_local(catalog, local)`）：网关把清单里的每个导出与
本地 `[backend_defaults.<其 backend>]` 合并成 `ExportConfig`：清单键优先；`kPerNodeBackendKeys`
**只**来自本地（清单里出现这些键时写入被拒绝，读取时告警忽略，向前兼容）。合并结果走与本地模式
同一条 `validate_config` + `ExportTable` 路径，后端的构造 / 能力校验 / `stat(path)` 逻辑完全相同
（`catalog_from_config(c)` 是反向：把一份本地配置的导出段取成清单，`import` 与摘要计算共用）。

```toml
# 网关本地 lightnfs.toml（每台各自一份）
[cluster]
enabled        = true
id             = "3f9c…-uuid"
shared_dir     = "/mnt/cephfs/.lightnfs-cluster"
mode           = "active-active"
node           = "gw1"
node_address   = "10.0.0.11:2049"
exports_source = "catalog"        # local（默认）| catalog
catalog_refresh = "auto"          # auto（默认）| manual

[backend_defaults.cephfs]         # 本机键，合并进清单里每个 cephfs 导出
conf    = "/etc/ceph/ceph.conf"
keyring = "/etc/ceph/ceph.client.gw1.keyring"
name    = "client.gw1"
# exports_source = "catalog" 时本地不得再有 [[export]]（EINVAL，避免两处事实来源）
```

```toml
# shared_dir/catalog.toml（集群一份，只由管理命令写）
[catalog]
version    = 7                    # 单调递增；每次提交 +1
updated_at = "2026-09-08T10:21:03Z"
updated_by = "gw2 uid=0"          # 接收管理命令的网关 + ctl 对端 uid（SO_PEERCRED）
comment    = "add /export/c on gw3"

[[export]]
path = "/export/a"; fsid = 1; backend = "cephfs"
nodes = ["gw1", "gw2", "gw3"]
[export.cephfs]
fs_name = "cephfs"; subdir = "/nfs/a"

[[export]]
path = "/export/c"; fsid = 3; backend = "cephfs"
nodes = ["gw3", "gw1"]
clients = ["10.0.0.0/8"]
[export.cephfs]
fs_name = "cephfs"; subdir = "/nfs/c"
```

## 11.3 共享存储布局与版本

在 09 §9.4 / 10 §10.3 的键空间旁加（`server/cluster_store.hpp` 顶部注释是权威）：

```
shared_dir/
  catalog.toml                 # 当前清单（整文件原子替换，含 [catalog] version）
  catalog.history/<version>.toml   # 每次提交前的快照，保留最近 32 份（回滚 / 审计）
  catalog.lock                 # 写者 O_EXCL 串行化（复用 ClusterStore::lock，陈旧锁回收）
  catalog.<node>               # "<applied_version> <digest> <applied_at_ms> <status>\n"
                               #   该网关已应用的版本与结果（ok | error:<text>），供 catalog status
                               #   节点名 toml / lock / history 是保留字（EINVAL），不会与上面三个撞名
```

- **读**：`ClusterStore::read_catalog()` → `{version, text}`；文件整体原子替换（`atomic_write_file`），
  读者永远看到完整的一版。轮询成本 = 每 tick 读一个几 KB 的文件，与 `fence.<node>` 同量级。
- **写**（`write_catalog(expected, text)`）：`text` 自带 `[catalog]` 头（含 `expected + 1` 的版本号，
  否则 EINVAL——存储层不改文档）。取 `catalog.lock` → 重读当前版本，≠ `expected` → `EAGAIN`
  （并发写入方之一让步重试；`expected = 0` 表示"还没有清单"）→ 把当前文件复制为
  `catalog.history/<version>.toml` → 原子写新文件 → 把历史修剪到最近
  `kCatalogHistoryKeep`（32）份 → 放锁，返回新版本号。
- **`catalog.<node>`** 替代 `exports.<node>` 作为"这台网关在跑哪一版"的登记；`exports.<node>`
  摘要在清单模式下仍写（值 = 合并后导出表的规范摘要，与本地模式同一算法），使**本地模式与清单
  模式混跑的过渡期**（§11.9）仍受 09 §9.3 的摘要校验保护。文件名 `catalog.*` 不与 `exports.`
  前缀过滤（`cluster_store.cpp` 的 `list_exports_digests`）、也不与 `fence.` / `epoch.` 冲突。

## 11.4 网关如何取用清单：启动、自动与手动跟进

**启动**（`exports_source = "catalog"`，`server/catalog_boot.{hpp,cpp}` 的 `load_catalog_exports`，
结果是一个 `CatalogBoot{present, version, catalog, exports}`）：

1. 读本地 TOML（不含 `[[export]]`，否则 EINVAL）→ 建 `ClusterStore` → `read_catalog()`。
2. 清单**存在** → `merge_with_local` → `validate_config` → `ExportTable::build`，与本地模式一样
   启动全部后端；写 `catalog.<node> = <version> ok`。
3. 清单**不存在**（全新集群）→ 以**空导出表**启动（清单模式下 `validate_config` 放开"exports
   非空"的要求）：伪根可挂、`cluster status` 报 `catalog=none`，等管理命令 `catalog import`
   或 `export add` 建出第一版后按下面的跟进规则加载。这就是集群的引导方式，不需要"先起
   一台特殊网关"。
4. 清单存在但**本机合并后校验失败**（比如本机缺 `[backend_defaults.cephfs]`、导出路径本机
   不存在）→ 启动失败并指出哪个 fsid 的哪个键——与本地配置写错时的行为一致。
   `--check-config` 走同一条路径（只读共享目录，不写），打印 `catalog: vN` 或
   `catalog: none yet (empty export table)`。
5. 一致性校验（09 §9.3）在清单模式下改为：读到的清单版本 vs 同伴 `catalog.<node>` 的版本只做
   **记录与告警**（同伴滚动应用中，短暂不一致是正常的），不再据此拒绝启动；`exports.<node>`
   摘要照写，供本地模式的同伴校验（§11.9）。

**跟进**：跟进器是 `server/catalog_applier.{hpp,cpp}` 的 `CatalogApplier`，**主备与多活共用同一
个**，两种模式只差"谁来敲它"。

- **轮询** `CatalogApplier::poll()` 挂在控制器的 `Hooks::after_tick` 上——每个 tick
  （`fence_lease`）末尾在**控制器自己的线程**上跑，只读共享目录、只投递，不做任何数据面动作。
  多活挂在 `FsClusterController`、主备挂在 09 的 `ClusterController`，因此 standby 也跟进
  （它的表在激活时随协议栈重建生效）。读到比已应用版本新的版本 → **auto**：投递一次应用到
  主循环（同时只有一个在飞）；**manual**：只记 `pending`，`cluster status` 与指标可见，不动。
  poll 顺带在退休队列非空时投递一次退休清扫（§11.6）。
- **手动触发**：`lightnfs-ctl cluster catalog apply`（`apply_now`：任意线程投递到主循环并等结果）、
  `lightnfs-ctl reload`、SIGHUP，三者最终都走同一个 `apply_latest()`；`reload` 顺带做本地文件的
  热重载（08 §8.1 的清单模式口径）。
- **应用流水线** `apply_latest()`（主循环线程）：读最新清单 → `parse_catalog` → `validate_catalog`
  → `merge_with_local` → 与已应用版本 `diff_catalog` → 组 `ExportSetPlan`（新增 / 重新启用 → 构造
  后端并 `start()`，走 `run_on_reactor(0)`，与启动相同；删除 / 禁用 → remove；`nodes` 与在线可变
  字段 → 就地 update）→ `ExportTable::apply`（§11.6）→ `sync_exports`（§11.7）→ 写
  `catalog.<node> = v ok`。**幂等**：表里已有的导出走 update 而不是重新 add。任一步失败 → 旧集
  原样不动（已经 `start()` 的后端再 `stop()` 回去）、写 `catalog.<node> = <old> error:<why>`、
  失败计数 +1、日志 warn；**不自动回滚清单**——管理员从 `catalog status` 看到哪台失败、为什么，
  修本机配置后 `apply`，或 `catalog rollback`。
- **失败的版本不会每个 tick 重试**：`kRetryEveryPolls`（30）个轮询才自动重试一次，免得一台配错
  的网关每 3 秒刷一次同样的错误；运维手动 `apply`，或下一个版本直接顶掉它。
- **同一 tick 里落后多个版本**只应用最新的——中间版本不逐个重放。

## 11.5 变更语义（按导出的每种变化）

| 变化 | 网关动作 | 客户端可见 |
|------|----------|------------|
| **新增导出** | 构造后端 + `start()`；加入导出集与伪根；控制器新增 `Fs{Remote}`；`nodes[0]`（活着且轮到）在下一 tick 按 10 §10.3 规则接管；failover 下活动网关直接服务 | 伪根 READDIR 多一项（伪根目录 change 属性递增），进入即 referral 到属主 |
| **`clients` / QoS / `readonly` / `squash` / `anon_*`** | 就地更新（`ExportTable::apply` 翻转条目里的原子标量，条目对象不换；`readonly` 变化对已打开写句柄的下一次 WRITE 生效，回 `ROFS`） | 立即 |
| **`nodes` 变化** | 控制器换新顺位；**当前属主不在新名单**且名单里有活着的网关 → 属主对该 fsid 执行 10 §10.7 的计划内迁移到名单里第一个活着的网关（`LNFS_REASON=migrate`）；无活着的候选 → 继续服务并告警，直到有 | 迁移语义：一个 grace 窗口 |
| **`disabled = true`** | 导出从服务集与伪根**移除**但 fsid 在清单里保留：属主 drain（释放围栏、`release_fsid`），无人接管；后端 `stop()` | 该导出的句柄回 `NFS4ERR_STALE` / v3 `ESTALE`（等同删除，这是"下线"） |
| **`disabled = false`** | 同新增 | 同新增 |
| **删除导出** | 同 `disabled`，随后清单里也没有它 | 同 `disabled` |
| **`path` / `backend` / 后端集群键变化** | **拒绝**（写入侧 EINVAL："remove and re-add, or use a new fsid"） | — |

删除有护栏：`export remove <fsid>` 默认要求该 fsid **当前无属主**（按发起网关看到的属主视图），
否则报 `fsid N is served by gw2 (disable it first, or --force)`。推荐顺序：`export set <fsid>
--disabled` → `catalog status` 看到无属主 → `export remove <fsid>`。`--force` 一步到位，属主收到新版
后自行 drain。

## 11.6 运行期可变的导出集（`ExportSet`）

"不重启地增删导出"要求导出表能换版，而多活明确**协议栈永不重建**
（`cluster_controller.hpp` 注释），借不到主备 `deactivate()/activate()` 那条路。手法是
**不可变快照 + RCU 发布**，与 `FsOwnerView`、`ExportEntry::clients_` 同一套（`core/config.hpp`）。

- `core::ExportSet`：一版导出集 = `vector<shared_ptr<ExportEntry>>`（按 fsid 升序、唯一）+ 由它
  建出的 `PseudoFs` + `by_fsid` / `for_mount_path` 两个查找；**不可变**，另带 `generation`
  （每次发布 +1）与 `epoch`（本次启动的 boot epoch）。
- `core::ExportTable` 是发布者：一个 `atomic<shared_ptr<const ExportSet>>`。`snapshot()` 取当前版，
  所有读者**每请求取一次快照**并持有到请求结束（`FileHandleCodec` 按 fsid 解码、引擎
  `resolve()`、`Mount3` 的 EXPORT 枚举、`ProtocolStack` 的伪根、指标遍历）。快照持
  `shared_ptr<ExportEntry>`，在途请求引用的条目即使被删也活到请求结束。写者只有启动路径与
  `apply(ExportSetPlan)`，都在主循环线程上。
- **换版** `ExportTable::apply(plan)`：`ExportSetPlan{add, update, remove}` 三段——`add` 是新 fsid
  （含重新启用的）、`update` 是同一 fsid 的在线可变字段（就地翻转条目里的原子标量，**不换条目
  对象**）、`remove` 是删除或禁用的 fsid。用 `ExportSetBuilder`（可从旧集起步）拼出新集，
  `finish(epoch, generation)` 排序、建伪根树、冻结，然后发布。
- **条目跨版共享**：未变化的导出在新旧集里是**同一个** `ExportEntry` 对象（`ExportSetBuilder::keep`
  沿用 `shared_ptr`），后端实例、fd 缓存、指标、QoS 桶、clients 原子指针全部延续；只有新增的
  导出构造新对象。这也保证控制器 `Fs::exp` 与 `PseudoFs` 里的裸指针在条目存续期内有效。
- **退休**：被删 / 禁用的条目进 `ExportTable` 的退休队列。后端 `stop()` 必须在主循环 / reactor 0
  上跑，不能放析构函数里，所以由 `CatalogApplier::retire_exports()` 在主循环上取走无人引用的条目、
  `stop()` 其后端再释放；仍被引用（在途请求或控制器）的条目留在队列里，超过
  `10 × [protocol] lease` 会周期性告警（收尾项 §5：还缺一个对应指标）。
- **伪根**：每版 `ExportSet` 自带一棵 `PseudoFs`；节点 id 是 `fnv64(path)`（`pseudofs.cpp` 的
  `stable_id`），跨版稳定，客户端缓存的伪根句柄继续可用；被删导出的穿越节点消失 → 该句柄回
  STALE。伪根目录的 change 属性是 `pseudo_change() = epoch << 32 | generation`——每次发布都变大，
  且跨重启不重复（高半是 boot epoch）。
- **v4 状态层不引用条目指针**（按 fsid 记录），删除导出时由 `release_fsid(fsid)` 清状态（10 §10.4）。
- 主备同样受益：standby 的表在线跟进，激活时重建的协议栈用的就是最新集。

## 11.7 控制器与清单

多活下 `FsClusterController` 的 per-fsid 状态机集合 `fs_` 必须跟着导出集走。应用流水线在发布
新集之后调 `sync_exports(const shared_ptr<const ExportSet>&)`（主循环线程，持 `mu_`）：

- **新 fsid** → 建一个 `Fs{exp, Role::kStandby}`，对引擎发布为 unowned，下一 tick 按 `our_turn()`
  决定是否接管（10 §10.3 的顺位规则原样适用，新导出不走任何特殊路径）；
- **`nodes` 变了**（与上次 `sync_exports` 记下的名单逐项比较）→ 换上新的 `Fs::exp`；若本网关是
  该 fsid 的属主**且不在新名单里**、而名单里有活着的网关 → 复用 10 §10.7 的
  `request_migrate(fsid, 名单里第一个活着的)`，客户端按迁移语义跟随；名单里没有活着的候选 →
  继续服务并告警，直到有；
- **fsid 消失**（删除或 `disabled = true`）→ 属主走
  `begin_draining(fsid, "removed from catalog", release=true)`，drain 完成后删掉 `Fs`；非属主
  直接删；
- 结束后 `publish()` 把新的属主视图推给引擎。

主备下没有 per-fsid 状态机，`sync_exports` 也就无从谈起（`CatalogApplier::Deps::fs_cluster` 为
空）：活动网关发布新集即生效，standby 的表在激活时随协议栈重建生效。

清单的轮询、`catalog.<node>` 登记、`catalog=` / `catalog_latest=` / `catalog_refresh=` /
`catalog_error=` 这些 `cluster status` 字段与 `lightnfs_cluster_catalog_*` 指标**都在
`CatalogApplier` 里**（它自己注册一个文本提供者），不在两个控制器的状态里——两种集群形态因此
共用同一份实现。

## 11.8 已知取舍与风险

- **fsid 复用**：句柄 HMAC 覆盖 fsid + ObjId，不含"导出代际"。删除 fsid 3 后再以别的后端 / 路径
  新增 fsid 3，旧客户端缓存的句柄可能解码到新后端的对象（HMAC 仍通过）。写入侧规则：历史
  32 版内出现过、且 `path`/`backend`/集群键不同的 fsid **拒绝复用**（`--force` 覆盖，文档明示
  风险）。彻底解法是把导出代际折进 HMAC 派生（`core/file_handle.cpp` 的 `key.bind`），留作演进。
- **应用不是原子的集群操作**：各网关各自在下一 tick 应用（auto）或等管理员（manual），期间
  版本不一致：新导出在旧版网关的伪根里暂时看不到；`nodes` 不一致时两台可能都认为"轮到我"，
  但围栏 CAS（`acquire_fs_fence` 在 `fence.lock` 下）仍是唯一仲裁，不会双属主。`catalog status`
  暴露每台的版本，`lightnfs_cluster_catalog_version` 可告警"落后超过 N 秒"。
- **本机校验失败只影响本机**：清单在发起网关通过了集群级校验（fsid / path 唯一、`nodes`
  语法、同卷同进退、后端类型存在），但本机 `[backend_defaults]` 缺失或路径不存在只能在应用
  时发现；该网关停在旧版并报错，其他网关照常前进。管理员用 `catalog status` 看到。
- **删除 / 禁用对客户端是硬中断**（STALE）：这是删除的本意；需要"不中断地换属主"用 `nodes` /
  `migrate`，需要"暂停服务但保留挂载"没有协议手段。
- **ctl 线协议要能表达带空格的参数**：`ctl.cpp` 的 `parse_command` 原本只按空白切分，`export add`
  的 `--path` / `--clients` / `--comment` 就写不出空格。现在它认 `"…"` 与 `\"` / `\\` 转义，ctl
  客户端对含空白 / 引号 / 反斜杠的参数自动加引号。`catalog import <file>` 的 `<file>` 始终是
  **网关侧**路径（与 `--config` 同义），文件内容不经线协议传输。
- **`readonly` / `squash` / `anon_*` 在线生效**只在清单模式下成立；本地模式的 `reload_dynamic`
  对这三者仍报"restart required"（零默认行为变化）。两种模式对同一个键的口径不同，是刻意的
  取舍——08 §8.1 把两份口径并排写出。
- **共享目录写放大**：每 tick 多读一个文件；写只在管理操作时发生。历史保留 32 份，几十 KB。
- **清单被手工改坏**：`read_catalog` 解析失败 → 所有网关停在各自的旧版并报错，不影响服务；
  `catalog rollback <version>` 从历史恢复（历史文件只由网关写，管理员不应手改）。

## 11.9 从本地模式迁移到清单模式

1. 集群仍在本地模式；在任一网关上 `lightnfs-ctl cluster catalog import /etc/lightnfs/lightnfs.toml`
   （本地模式下允许 `import` / `show` / `status`，只是本机不取用）：网关剥掉本机键写出 v1。
   或用离线形态 `lightnfs-ctl catalog import --shared-dir=<dir> --from-local=<file>`（§11.10，
   不需要运行中的网关，适合首次引导 / 全部网关都起不来时的救援）。
2. 逐台改本地 TOML：删 `[[export]]`、加 `exports_source = "catalog"` 与 `[backend_defaults.*]`，用
   `scripts/cluster_roll.sh evacuate/restore` 滚动重启。过渡期两种模式混跑：清单模式网关的
   `exports.<node>` 摘要与本地模式网关相同（同一份内容、同一算法），09 §9.3 校验仍通过；若
   有人此时改了清单，本地模式网关**下次启动**时摘要不一致而拒绝入集群——这正是想要的保护。
3. 全部切换后，`catalog status` 显示每台的版本，本地文件里不再有导出。
4. 回退：把 `[[export]]` 放回本地文件、`exports_source = "local"`，逐台重启即可；清单文件留着
   无害。

## 11.10 管理命令

全部经 `lightnfs-ctl` 的 unix socket 对**任一**网关发出（写清单不要求它是属主），`--json` 同形：

| 命令 | 作用 |
|------|------|
| `cluster catalog show` | 当前清单：版本、更新者、每导出一行（fsid path backend nodes disabled clients …） |
| `cluster catalog status` | 每台网关已应用版本 / 状态（`catalog.<node>`）、心跳、`pending`；一眼看到谁落后、谁失败 |
| `cluster catalog history` | 历史版本列表（版本、时间、更新者、comment） |
| `cluster catalog diff [<v1>] [<v2>]` | 两版之间的导出级差异（默认：当前 vs 本网关已应用） |
| `cluster catalog import <网关侧文件>｜--from-local=<文件> [--dry-run] [--comment=…]` | 用一份 TOML 的导出段整体替换清单（剥掉本机键、集群级校验、CAS 写入）；文件可以是本地配置或清单文档，`--from-local` 声明前者并拒绝后者 |
| `cluster catalog rollback <version> [--comment=…]` | 把历史某版作为新版提交（版本号仍递增） |
| `cluster catalog apply` | 本网关立即应用最新版（`catalog_refresh = manual` 的触发；auto 下等价于"不等下一 tick"） |
| `cluster export list` | = `catalog show` 的导出行 |
| `cluster export add --path P --fsid N [--backend B] [--nodes a,b] [--clients c1,c2] [--readonly] [--squash root\|all\|none] [--anon-uid N] [--anon-gid N] [--read-bps N] [--write-bps N] [--iops N] [--opt k=v …] [--disabled] [--force] [--dry-run] [--comment T]` | 新增导出；`--opt` 是后端集群键（本机键被集群级校验拒）；`--force` 越过 fsid 复用规则 |
| `cluster export set <fsid> …` | 同一组在线可变标志；`--readonly=false` / `--disabled=false` 清除；`--path` / `--backend` / `--opt` 被拒（"remove and re-add, or use a new fsid"） |
| `cluster export remove <fsid> [--force]` | 删除（护栏见 §11.5） |

标志 `--k=v` 与 `--k v` 都收；三个 `export` 子命令都答
`catalog vN committed (was vM): export fsid=X added|updated|removed: <diff>`，`--dry-run` 只报不写。
ctl 客户端把 `cluster export …` 整行原样转发给服务端解析（不经自己的标志解析器）。

**离线形态**：`lightnfs-ctl catalog <show|status|history|diff|import|rollback> --shared-dir=<dir>`
不连任何网关，在 ctl 进程里对共享目录跑同一批命令体——首次引导与"所有网关都起不来"的救援用。
文案与 `--json` 和网关上完全一致，审计头记 `updated_by = "offline uid=<getuid>"`。`apply` **不在**
离线子命令树里：应用一版是运行中的网关的事。

写入路径（发起网关）：`read_catalog()` → `parse_catalog` → 施加变更 → **集群级校验**
`validate_catalog`（fsid / path 唯一且不互为前缀、`nodes` 语法与去重、后端类型存在、同卷同进退
10 §10.6、本机键不得出现、fsid 复用规则）→ `serialize_catalog`（规范格式，`[catalog]` 头由网关
填）→ `write_catalog(expected)`，`EAGAIN` 时**重读当前版再重新施加变更**并重试，最多 3 次 →
返回新版本号。因此并发的单导出编辑不会互相覆盖。发起网关自己不特殊：它也在下一 tick
（或 `apply`）才应用自己刚写的版本。审计头 `updated_by = "<node> uid=<SO_PEERCRED>"`。

`cluster status` 网关行加 `catalog=<applied> catalog_latest=<seen> catalog_refresh=auto|manual
catalog_error=-|<text>`；指标 `lightnfs_cluster_catalog_version`（已应用）、
`lightnfs_cluster_catalog_latest_version`（共享目录里的）、`lightnfs_cluster_catalog_applies_total`、
`lightnfs_cluster_catalog_apply_failures_total`。

## 11.11 代码地图与验证

| 位置 | 职责 |
|------|------|
| `core/config.{hpp,cpp}` | `[cluster] exports_source / catalog_refresh`、`[backend_defaults.<backend>]` 解析与校验；清单模式下本地 `[[export]]` 为 EINVAL、空导出表放行；`ExportSet` / `ExportSetBuilder` / `ExportSetPlan` / `ExportTable::{snapshot,apply,take_retired}`（§11.6） |
| `core/catalog.{hpp,cpp}` | `Catalog` 文档模型；`parse_catalog` / `serialize_catalog`（规范格式，往返稳定）/ `merge_with_local` / `catalog_from_config` / `validate_catalog`（集群级）/ `diff_catalog` |
| `core/pseudofs.{hpp,cpp}` | 每版 `ExportSet` 自带的伪根树；`stable_id` 保证节点 id 跨版稳定 |
| `server/cluster_store.{hpp,cpp}` | `read_catalog` / `write_catalog(expected)` 的 CAS 提交与历史修剪（`kCatalogHistoryKeep = 32`）/ `list_catalog_history` / `read_catalog_history` / `put_catalog_applied` / `list_catalog_applied` |
| `server/catalog_boot.{hpp,cpp}` | 启动期 `load_catalog_exports` → `CatalogBoot{present, version, catalog, exports}`；`--check-config` 共用 |
| `server/catalog_applier.{hpp,cpp}` | `CatalogApplier`：`poll`（控制器 tick 上）/ `apply_latest` / `apply_now` / `retire_exports` / `status` / `append_metrics`；主备与多活共用 |
| `server/cluster_controller.{hpp,cpp}` | `Hooks::after_tick` 挂轮询；`FsClusterController::sync_exports`（§11.7） |
| `server/daemon.cpp` | 清单模式启动、装配 `CatalogApplier`（`post` → 主循环、`start/stop_backend` → reactor 0）、`reload` 的清单口径（08 §8.1） |
| `server/ctl.cpp`、`server/ctl_catalog.{hpp,cpp}` | `parse_command` 的引号与转义；`cluster catalog …` / `cluster export …` 的命令体与 `--json`；`cluster status` 的四个 `catalog*` 字段 |
| `tools/lightnfs_ctl.cpp` | ctl 命令树、`cluster export …` 整行转发、离线 `catalog … --shared-dir` |

**验证**：

- **跨进程**：`scripts/accept_active_active_local.sh` 把导出来源做成外层循环
  （`LNFS_EXPORTS="local catalog"`），清单模式下本地文件里一个 `[[export]]` 都没有，两个导出由
  启动前的离线 `lightnfs-ctl catalog import --from-local` 引导为 v1；既有的 status / v4moved /
  roll / crash 四段在两种来源下逐字同样通过。专门的 `catalog` 段（配合 `lnfs_accept_client` 的
  `v4catalog` 模式）覆盖：三台 `catalog status` 在同一版 → 在线 `export add` 后伪根 READDIR 多
  一项且伪根 change 严格变大 → 非属主上 READ 回 MOVED 且 `fs_locations` 非空（**要轮询**：没人
  拿到围栏之前答的是 DELAY）→ `export set --nodes` 把属主踢出名单后原属主的 SEQUENCE 置
  `SEQ4_STATUS_LEASE_MOVED` → `--disabled=true` 后旧句柄回 STALE、名字从 READDIR 消失 →
  `remove` 的属主护栏与 `--force` → 一台改 `catalog_refresh = manual` 后停在旧版、
  `cluster catalog apply` 追平 → `catalog rollback 1` 全部回到 v1。Release 与 ASAN 各一轮。
- **单元**：`tests/test_catalog.cpp`（解析 / 序列化往返 / `merge_with_local` / `validate_catalog` /
  `diff_catalog`，以及配置侧的新键与清单模式的放行 / 拒绝）、`tests/test_export_set.cpp`
  （快照稳定性、条目跨版共享、伪根随集走、`apply` 的三段与退休队列，§11.6）、
  `tests/test_cluster_store.cpp`（`write_catalog` 的 CAS、历史修剪、`catalog.<node>` 的保留节点名）、
  `tests/test_daemon_lifecycle.cpp`（清单模式启动：有清单 / 无清单空表引导 / 本机合并失败）、
  `tests/test_cluster_controller.cpp`（`CatalogApplier` 的应用流水线、失败回退与
  `kRetryEveryPolls` 重试节奏、退休队列；`sync_exports` 的新增 / `nodes` 变更 / 删除；陈旧 owner
  记录不复活未列名节点）、`tests/test_ctl.cpp`（两组 ctl 命令的文本与 `--json`、离线形态）、
  `tests/test_metrics.cpp`（`lightnfs_cluster_catalog_*`）；共享目录用
  `tests/mem_cluster_store.hpp`。
