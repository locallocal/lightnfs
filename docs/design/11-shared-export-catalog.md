# 11. 共享导出清单（集群级集中式导出配置）——设计方案

> 状态：**设计稿（2026-09-08），未开始实施**。实施步骤见 [12 册](12-shared-export-catalog-steps.md)。
> 本册建立在 [10 册](10-multi-gateway-active-active.md) 多活与 [09 册](09-multi-gateway-failover.md)
> 主备之上：`ClusterStore`（09 §9.4）、导出表摘要一致性校验（09 §9.3）、`FsClusterController`
> 与 per-fsid 围栏 / 属主视图（10 §10.3）、`cluster migrate`（10 §10.7）、既有的热重载子集
> （08 §8.1，`ExportTable::reload_dynamic`）均已实现，本册只在其上加"导出配置的来源与生命周期"。

## 11.1 问题与目标

**现状**（10 §10.10）：多活集群里每台网关各带一份本地 TOML，其中 `[[export]]` 段（path / fsid /
backend / squash / readonly / anon / 后端子表 / `nodes`）**在全集群必须逐字相同**——启动时用
`exports.<node>` 摘要互相校验，不一致的网关拒绝入集群。于是：

- **加一个导出 = 改 N 份文件 + 重启 N 台网关**：`ExportTable` 启动后不可增删（`core/config.cpp`
  `reload_dynamic` 只接受 clients 白名单与 QoS，其余一律"restart required"）。
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
5. **零默认行为变化**：`[cluster] exports_source = "local"`（默认）保持今天的一切；本册全部改动
   挂在 `exports_source = "catalog"` 之后；failover 与 active-active 都可用清单。

**非目标**：

- 不做配置中心 / 服务发现：清单仍只经 `ClusterStore` 走共享目录，**不加网关间 RPC**（沿用 09/10
  "lightnfs 不选主、不通信"的原则）。
- 不集中 `[server]`、`[cluster]` 身份、日志、TLS、资源限制等**本机**配置——那些本来就该按机器配。
- 不在线改一个 fsid 的 `path` / `backend` / 后端标识键（句柄编码绑定 fsid + 后端 ObjId，见 §11.8）。
- 不做多写者并发合并：清单写入是"读-改-写 + 版本 CAS"，并发写入方之一失败重试。

## 11.2 什么进清单、什么留本地

| 键 | 现在 | 本册 | 理由 |
|----|------|------|------|
| `[[export]] path / fsid / backend / readonly / squash / anon_uid / anon_gid` | 本地，进摘要 | **清单** | 导出身份，全集群必须一致 |
| `[[export]] nodes` | 本地，进摘要，改动需重启 | **清单**，在线生效 | 属主顺位是集群决策 |
| `[[export]] clients / read_bps / write_bps / iops` | 本地，热重载 | **清单**，在线生效 | 客户端从任一网关看到的策略应一致 |
| `[[export]] disabled`（新） | — | **清单** | 保留 fsid 但下线（维护 / 删除前置） |
| `[export.<backend>]` 集群键（volume / fs_name / subdir / mount …） | 本地，进摘要 | **清单** | 与导出身份同级 |
| `[export.<backend>]` 本机键 `kPerNodeBackendKeys`（conf / keyring / id / user / name / log_file / fd_cache / mon_host） | 本地，不进摘要 | **本地** `[backend_defaults.<backend>]` | 凭据、日志路径、缓存大小按机器配 |
| `[server]`、`[cluster]`、`[log]`、`[protocol]` 等 | 本地 | 本地 | 本机运行参数 |

**合并规则**（`core/catalog.cpp` 的 `merge_export`）：网关把清单里的每个导出与本地
`[backend_defaults.<其 backend>]` 合并成 `ExportConfig`：清单键优先；`kPerNodeBackendKeys` **只**来自
本地（清单里出现这些键时写入被拒绝，读取时告警忽略，向前兼容）。合并结果走今天的
`validate_config` + `ExportTable` 路径，后端的构造 / 能力校验 / `stat(path)` 逻辑一行不改。

```toml
# 网关本地 lightnfs.toml（每台各自一份）
[cluster]
enabled        = true
id             = "3f9c…-uuid"
shared_dir     = "/mnt/cephfs/.lightnfs-cluster"
mode           = "active-active"
node           = "gw1"
node_address   = "10.0.0.11:2049"
exports_source = "catalog"        # 新键：local（默认）| catalog
catalog_refresh = "auto"          # 新键：auto（默认）| manual

[backend_defaults.cephfs]         # 新段：本机键，合并进清单里每个 cephfs 导出
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

在 10 §10.3 的键空间旁加：

```
shared_dir/
  catalog.toml                 # 当前清单（整文件原子替换，含 [catalog] version）
  catalog.history/<version>.toml   # 每次提交前的快照，保留最近 32 份（回滚 / 审计）
  catalog.lock                 # 写者 O_EXCL 串行化（复用 ClusterStore::lock，陈旧锁回收）
  catalog.<node>               # "<applied_version> <digest> <applied_at_ms> <status>\n"
                               #   该网关已应用的版本与结果（ok | error:<text>），供 catalog status
```

- **读**：`ClusterStore::read_catalog()` → `{version, text}`；文件整体原子替换（`atomic_write_file`），
  读者永远看到完整的一版。轮询成本 = 每 tick 读一个几 KB 的文件，与 `fence.<node>` 同量级。
- **写**（`write_catalog(expected_version, text, meta)`）：取 `catalog.lock` → 重读当前版本，
  ≠ `expected_version` → `EAGAIN`（并发写入方之一让步重试）→ 把当前文件复制为
  `catalog.history/<version>.toml` → 原子写新文件（`version + 1`）→ 修剪历史 → 放锁。
- **`catalog.<node>`** 替代 `exports.<node>` 作为"这台网关在跑哪一版"的登记；`exports.<node>`
  摘要在清单模式下仍写（值 = 合并后导出表的规范摘要，与今天算法相同），使**本地模式与清单
  模式混跑的过渡期**（§11.9）仍受 09 §9.3 的摘要校验保护。文件名 `catalog.*` 不与 `exports.`
  前缀过滤（`cluster_store.cpp` `list_exports_digests`）冲突。

## 11.4 网关如何取用清单：启动、自动与手动跟进

**启动**（`exports_source = "catalog"`）：

1. 读本地 TOML（不含 `[[export]]`）→ 建 `ClusterStore` → `read_catalog()`。
2. 清单**存在** → 合并本地 `[backend_defaults]` → `validate_config` → `ExportTable::build`，与今天
   一样启动全部后端；写 `catalog.<node> = <version> ok`。
3. 清单**不存在**（全新集群）→ 以**空导出表**启动（`validate_config` 在清单模式下放开
   "exports 非空"的要求）：伪根可挂、`cluster status` 报 `catalog=none`，等管理命令 `catalog import`
   或 `export add` 建出第一版后，按下面的跟进规则加载。这就是集群的引导方式，不需要"先起
   一台特殊网关"。
4. 清单存在但**本机合并后校验失败**（比如本机缺 `[backend_defaults.cephfs]`、导出路径本机
   不存在）→ 启动失败并指出哪个 fsid 的哪个键——与今天本地配置错误的行为一致。
5. 一致性校验（09 §9.3）在清单模式下改为：读到的清单版本 vs 同伴 `catalog.<node>` 的版本只做
   **记录与告警**（同伴滚动应用中，短暂不一致是正常的），不再据此拒绝启动；`exports.<node>`
   摘要照写，供本地模式的同伴校验（§11.9）。

**跟进**（两种模式共用一条流水线，只差"谁触发"）：

- `FsClusterController::tick()`（每 `fence_lease`，已有）末尾 `read_catalog()` 的版本号；比已应用
  版本新 → **auto**：把"应用 v"投递到主循环（`Hooks::post`，与接管一样不在 tick 线程做数据面
  动作）；**manual**：只记 `pending_version`，`cluster status` 与指标可见，不动。failover 模式下
  没有 `FsClusterController`，由 09 的 `ClusterController` 围栏线程做同一件事（standby 也应用，
  它的表在激活时随协议栈重建生效）。
- 手动触发：`lightnfs-ctl cluster catalog apply`、`lightnfs-ctl reload`、SIGHUP 三者都走主循环上的
  同一 `apply_catalog(v)`；`reload` 顺带做今天的本地文件热重载。
- **应用流水线** `apply_catalog(v)`（主循环线程）：读清单 v → 合并 → `validate_config` →
  计算与当前导出集的差异（§11.5）→ 新导出构造后端并 `start()`（`run_on_reactor(0)`，与启动
  相同）→ 发布新导出集（§11.6）→ 控制器 `sync_exports()`（§11.7）→ 退休被删导出（§11.6）→
  写 `catalog.<node> = v ok`。任一步失败 → 保持旧集不变、写 `catalog.<node> = <old> error:<why>`、
  指标 +1、日志 warn；**不自动回滚清单**——管理员从 `catalog status` 看到哪台失败、为什么，
  修本机配置后 `apply`，或 `catalog rollback`。
- **同一 tick 里有多个新版本**只应用最新的；应用是幂等的（按内容 diff）。

## 11.5 变更语义（按导出的每种变化）

| 变化 | 网关动作 | 客户端可见 |
|------|----------|------------|
| **新增导出** | 构造后端 + `start()`；加入导出集与伪根；控制器新增 `Fs{Remote}`；`nodes[0]`（活着且轮到）在下一 tick 按 10 §10.3 规则接管；failover 下活动网关直接服务 | 伪根 READDIR 多一项（伪根目录 change 属性递增），进入即 referral 到属主 |
| **`clients` / QoS / `readonly` / `squash` / `anon_*`** | 就地更新（`readonly`/`squash`/`anon_*` 从"restart required"改为在线：它们只是每请求读的标量；`readonly` 变化对已打开写句柄的下一次 WRITE 生效，回 `ROFS`） | 立即 |
| **`nodes` 变化** | 控制器换新顺位；**当前属主不在新名单**且名单里有活着的网关 → 属主对该 fsid 执行 10 §10.7 的计划内迁移到名单里第一个活着的网关（`LNFS_REASON=migrate`）；无活着的候选 → 继续服务并告警，直到有 | 迁移语义：一个 grace 窗口 |
| **`disabled = true`** | 导出从服务集与伪根**移除**但 fsid 在清单里保留：属主 drain（释放围栏、`release_fsid`），无人接管；后端 `stop()` | 该导出的句柄回 `NFS4ERR_STALE` / v3 `ESTALE`（等同删除，这是"下线"） |
| **`disabled = false`** | 同新增 | 同新增 |
| **删除导出** | 同 `disabled`，随后清单里也没有它 | 同 `disabled` |
| **`path` / `backend` / 后端集群键变化** | **拒绝**（写入侧 EINVAL："remove and re-add, or use a new fsid"） | — |

删除有护栏：`export remove <fsid>` 默认要求该 fsid **当前无属主**（按发起网关看到的属主视图），
否则报 `fsid N is served by gw2 (disable it first, or --force)`。推荐顺序：`export set <fsid>
--disabled` → `catalog status` 看到无属主 → `export remove <fsid>`。`--force` 一步到位，属主收到新版
后自行 drain。

## 11.6 运行期可变的导出集（`ExportSet`，最主要的代码改动）

今天 `ExportTable` 是一个无锁、无删除、启动后不变的 `vector<unique_ptr<ExportEntry>>`；
`PseudoFs` 在 `ProtocolStack` 构造时从导出路径建树、无重建接口；`FileHandleCodec` / 两个引擎 /
`Mount3` / 指标 / 控制器都持裸指针或直接遍历。active-active 明确"协议栈永不重建"
（`cluster_controller.hpp` 注释），所以不能借 failover 的 `deactivate()/activate()` 换表。

设计：**不可变快照 + RCU 发布**，与 `FsOwnerView`、`ExportEntry::clients_` 同一手法。

- `core::ExportSet`：一版导出集 = `vector<shared_ptr<ExportEntry>>`（按 fsid 有序）+ 由它建出的
  `PseudoFs` + `by_fsid` / `for_mount_path` 索引；**不可变**。
- `core::ExportTable` 变成发布者：`std::atomic<std::shared_ptr<const ExportSet>>`；`snapshot()` 取当前
  版；`publish(new_set)` 换版。所有读者**每请求取一次快照**并持有到请求结束（`FileHandleCodec`
  按 fsid 解码、引擎 `resolve()`、`Mount3` 的 EXPORT 枚举、`ProtocolStack` 的伪根、指标遍历）。
  快照持有 `shared_ptr<ExportEntry>`，因此在途请求引用的条目在被删后仍活到请求结束。
- **条目跨版共享**：未变化的导出在新旧集里是**同一个** `ExportEntry` 对象（`shared_ptr` 复用），
  后端实例、fd 缓存、指标、QoS 桶、clients 原子指针全部延续；只有新增的导出构造新对象。这也
  保证控制器 `Fs::exp` 与 `PseudoFs` 里的裸指针在条目存续期内有效。
- **退休**：被删 / 禁用的条目从新集消失后，旧快照的最后一个持有者释放时条目析构；后端
  `stop()` 必须在主循环 / reactor 0 上跑，所以不放析构函数里——应用流水线把它放进"退休
  队列"，每 tick 检查 `use_count() == 1` 后 `stop()` 并释放（与 `retired_clients_` 的思路一致，
  但会真正回收）。
- **伪根**：每版 `ExportSet` 自带一棵 `PseudoFs`；节点 id 是 `fnv64(path)`（`pseudofs.cpp`
  `stable_id`），跨版稳定，客户端缓存的伪根句柄继续可用；被删导出的穿越节点消失 → 该句柄回
  STALE；伪根目录的 change 属性用"集版本号 × 2^32 + 启动 epoch"保证单调递增。
- **v4 状态层不引用条目指针**（按 fsid 记录），删除导出时由 `release_fsid(fsid)` 清状态（已有）。
- failover 模式同样受益：standby 的表在线跟进，激活时重建的协议栈用的就是最新集。

## 11.7 控制器与清单

`FsClusterController` 的 `fs_` 目前在构造时按导出表一次建成、不可增删。加 `sync_exports(const
ExportSet&)`（主循环线程调，持 `mu_`）：

- 新 fsid → `fs_[fsid] = Fs{exp, Role::kStandby}`，视图 unowned，下一 tick 按 `our_turn()` 接管；
- `nodes` 变了 → 只换 `Fs::exp`（新条目）；若本网关是属主且不在新名单，且名单里有活着的
  → 复用 `request_migrate(fsid, 首个活着的)`；
- fsid 消失（删除 / 禁用）→ 属主 `begin_draining(fsid, "removed from catalog", release=true)`；
  非属主直接删 `fs_[fsid]`；drain 完成后删；
- `publish()` 照常把结果推给引擎视图。
- tick 末尾的清单轮询（§11.4）与 `catalog.<node>` 登记也在这里；`FsState` 加 `catalog_version`
  供 `cluster status`。

failover 的 `ClusterController` 只需轮询 + 投递应用，没有 per-fsid 逻辑。

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
- **ctl 线协议无引号**（`ctl.cpp` `parse_command` 按空白切分）：`export add` 的 `--path` / `--clients`
  含空格无法表达。12 册 D1 给 `parse_command` 加双引号 + 反斜杠转义，ctl 客户端按需加引号；
  `catalog import <file>` 的 `<file>` 是**网关侧**路径（同今天 `--config`），不经线协议传内容。
- **`readonly` / `squash` / `anon_*` 改为在线生效**是行为变化，只在清单模式下启用；本地模式
  的 `reload_dynamic` 保持"restart required"（零默认行为变化）。
- **共享目录写放大**：每 tick 多读一个文件；写只在管理操作时发生。历史保留 32 份，几十 KB。
- **清单被手工改坏**：`read_catalog` 解析失败 → 所有网关停在各自的旧版并报错，不影响服务；
  `catalog rollback <version>` 从历史恢复（历史文件只由网关写，管理员不应手改）。

## 11.9 从本地模式迁移到清单模式

1. 集群仍在本地模式；在任一网关上 `lightnfs-ctl cluster catalog import /etc/lightnfs/lightnfs.toml`
   （本地模式下允许 `import` / `show` / `status`，只是本机不取用）：网关剥掉本机键写出 v1。
   或用离线形态 `lightnfs-ctl catalog import --shared-dir <dir> <file>`（12 册 D3，不需要运行中的
   网关，适合首次引导）。
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
| `cluster catalog import <gateway-side-file> [--dry-run] [--comment …]` | 用一份 TOML 整体替换清单（剥掉本机键、校验、CAS 写入） |
| `cluster catalog rollback <version>` | 把历史某版作为新版提交（版本号仍递增） |
| `cluster catalog apply` | 本网关立即应用最新版（manual 模式的触发；auto 下等价于"不等下一 tick"） |
| `cluster export list` | = `catalog show` 的导出部分 |
| `cluster export add --path P --fsid N --backend B --nodes a,b,c [--clients …] [--readonly] [--squash …] [--anon-uid …] [--read-bps …] [--opt key=value …] [--comment …]` | 新增导出；`--opt` 是后端集群键；本机键被拒 |
| `cluster export set <fsid> [--nodes …] [--clients …] [--readonly=bool] [--squash …] [--read-bps …] [--disabled=bool] …` | 改在线可变字段；`--path` / `--backend` / `--opt` 被拒 |
| `cluster export remove <fsid> [--force]` | 删除（护栏见 §11.5） |

写入路径（发起网关）：`read_catalog()` → 解析 → 施加变更 → **集群级校验**（fsid / path 唯一且
不互为前缀、`nodes` 语法与去重、后端类型存在、同卷同进退 10 §10.6、本机键不得出现、fsid
复用规则）→ 序列化（规范格式，`[catalog]` 头由网关填）→ `write_catalog(expected)`，`EAGAIN`
重试 3 次 → 返回新版本号。发起网关自己不特殊：它也在下一 tick（或 `apply`）应用。

`cluster status` 网关行加 `catalog=<applied> catalog_latest=<seen> catalog_refresh=auto|manual
catalog_error=-|<text>`；指标 `lightnfs_cluster_catalog_version`（已应用）、
`lightnfs_cluster_catalog_latest_version`（共享目录里的）、`lightnfs_cluster_catalog_applies_total`、
`lightnfs_cluster_catalog_apply_failures_total`。

## 11.11 实现阶段（详见 12 册）

| 阶段 | 交付 | 依赖 |
|------|------|------|
| A 清单文档与存储（无行为变化） | `exports_source` / `catalog_refresh` / `[backend_defaults]` 解析；`core/catalog.*` 解析 / 序列化 / 合并 / 集群级校验；`ClusterStore` 的 `read_catalog / write_catalog / history / catalog.<node>` | 10 册 |
| B 运行期可变导出集 | `ExportSet` 快照 + RCU 发布；读者改取快照；新增 / 就地更新 / 退休；控制器 `sync_exports` | — |
| C 启动与跟进 | 清单模式启动（含空表引导）；tick 轮询 + auto 投递 / manual 挂起；`apply` / `reload` / SIGHUP；`cluster status` 字段与指标 | A B |
| D 管理命令 | ctl 线协议引号；`cluster catalog …` / `cluster export …`；离线 `catalog import --shared-dir` | A C |
| E 验收与文档 | 三实例脚本加"清单"段（引导 → 在线加导出 → referral 可见 → 改 `nodes` 迁移 → 删除 STALE）；08 / 10 / deployment / README | 全部 |

**可用性里程碑**：A + C（以"新增 / 删除仍 restart required"的桩应用器）+ D 已经交付"集中配置
+ 管理命令 + `nodes` / clients / QoS 在线"；B 落地后才有"不重启增删导出"。若 B 延期，C 的
应用器先按 `reload_dynamic` 的口径报 `restart required`，不阻塞前三者。
