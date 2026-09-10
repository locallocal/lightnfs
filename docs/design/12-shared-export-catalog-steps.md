# 12. 共享导出清单——实现步骤拆分

> 状态：**实施中**（阶段 A、B、C 已完成；D1、D2 已完成）。本册把 [11 册](11-shared-export-catalog.md) 的方案拆成可独立合并、
> 可独立验证的步骤；每步给出改动点（带现有代码锚点）、接口形态、测试与验收标准。11 册回答
> "做什么、为什么"，本册只回答"按什么顺序、改哪里、怎么证明做对了"。体例沿用 09 / 10 册的
> 实施计划（原 10、12 册，完成后撤下，见 git 历史）；本册完成后同样撤下，未闭环项收进
> `docs/toto/shared-export-catalog-followups.md`。
>
> 前置：10 册全部已实现：`ClusterStore` 多活键空间（`server/cluster_store.*`）、
> `FsClusterController`（`server/cluster_controller.*`）、`FsOwnerView`（`core/fs_owner_view.hpp`）、
> `cluster migrate` / `cluster exports`（`server/ctl.cpp`）、`scripts/accept_active_active_local.sh`。

## 12.0 总原则

1. **默认零行为变化**：新键 `[cluster] exports_source` 默认 `local`，今天的一切逐字节不变；本册
   全部改动挂在 `exports_source = "catalog"` 之后。回归门：现有 `lnfs_tests` +
   `scripts/accept_m6_local.sh`（单网关）+ `scripts/accept_failover_local.sh`（主备）+
   `scripts/accept_active_active_local.sh`（多活）在每一步合并后原样通过。
2. **一步一个 PR，可单独回滚**：步骤之间只允许向前依赖（§12.1）。
3. **先做快照、再做变更**：阶段 B 先把导出表改成不可变快照 + RCU 发布而**不改任何行为**（B1
   合并后只有一版快照、永不换版），换版能力（B2/B3）在此之上单独证明。
4. **每步自带测试**：单元测试进 `tests/`（共享目录用 `tests/mem_cluster_store.hpp`），跨进程场景进
   `lnfs_accept_client` + `scripts/`；没有测试的步骤不合并。
5. **可用性里程碑优先**：A → C → D 先交付"集中配置 + 管理命令 + `nodes` / clients / QoS 在线"，
   C 的应用器先以 `reload_dynamic` 的口径对增删导出报 `restart required`；B 落地后 C2 换成真正
   的换版。B 与 A/C/D 并行开发无冲突。

### 本册对 11 册的实现细化

| 点 | 决定 | 依据 |
|----|------|------|
| 清单文件格式 | TOML，`[[export]]` / `[export.<backend>]` 语法与本地文件**完全相同**，外加 `[catalog]` 头；解析复用 `parse_config` 的导出段分支（抽成 `parse_export_sections`） | 管理员可以把本地文件直接 `import`；不引入第二种语法 |
| 版本号存哪 | 在 `catalog.toml` 的 `[catalog] version` 里，**不**单独放 version 文件 | 一个文件原子替换 = 读者永远看到自洽的一版，无需双读校验 |
| 轮询在哪 | `FsClusterController::tick()` 末尾（多活）/ `ClusterController` 围栏线程（failover），周期 = `fence_lease`；不加新线程 | 已有周期性共享目录 IO，加一次小文件读 |
| 应用在哪个线程 | 主循环（`Hooks::post` → `MainLoop::post`，`daemon.cpp`）；后端 `start()`/`stop()` 用 `run_on_reactor(0)` | 与接管 / 启动同一线程纪律；`reload` 今天在 ctl reactor 跑文件 IO，本册顺手把它也投递到主循环 |
| 条目跨版共享 | `shared_ptr<ExportEntry>`，未变化的导出复用同一对象 | 后端实例 / fd 缓存 / 指标 / QoS 桶 / clients 指针延续；控制器与伪根的裸指针不失效 |
| 退休条目何时 `stop()` | 主循环上的退休队列，每 tick 检查 `use_count()==1` 后 `stop()`；超过 10 × lease 仍被持有则告警 | `stop()` 是协程，不能放析构 |
| `readonly`/`squash`/`anon_*` 在线生效 | 仅清单模式；本地模式 `reload_dynamic` 口径不变 | 零默认行为变化 |
| ctl 引号 | `parse_command` 支持 `"…"` 与 `\"` / `\\`；ctl 客户端对含空白 / 引号的参数自动加引号 | `--path` / `--clients` / `--comment` 需要 |
| `import` 的文件 | 网关侧路径（同 `--config`）；离线形态 `lightnfs-ctl catalog … --shared-dir` 直接写共享目录 | 线协议不传文件内容 |
| 一致性校验 | 清单模式：只记录 / 告警版本差异，不拒绝启动；`exports.<node>` 摘要照写 | 11 §11.4 第 5 条、§11.9 过渡期保护 |

## 12.1 阶段与依赖

| 阶段 | 步骤 | 交付物 | 依赖 | 11 册阶段 |
|------|------|--------|------|-----------|
| A 清单文档与存储 | A1 配置键 | `exports_source` / `catalog_refresh` / `[backend_defaults.<backend>]`；清单模式下本地 `[[export]]` 为 EINVAL、空导出表放行 | — | A |
| | A2 `core/catalog.*` | `Catalog{version, meta, exports}`、`parse_catalog` / `serialize_catalog` / `merge_with_local` / `validate_catalog` / `diff_catalog` | A1 | A |
| | A3 `ClusterStore` 清单键 | `read_catalog` / `write_catalog(expected)` / `list_catalog_history` / `read_catalog_history` / `put_catalog_applied` / `list_catalog_applied` | — | A |
| B 运行期可变导出集 | B1 `ExportSet` 快照 + RCU 发布（无行为变化） | `core::ExportSet`；`ExportTable::snapshot()/publish()`；读者改取快照；`PseudoFs` 随集 | — | B |
| | B2 换版：新增 / 就地更新 / 退休 | `ExportTable::apply(ExportSetPlan)`；退休队列；伪根 change 单调 | B1 | B |
| | B3 控制器 `sync_exports` | 新 fsid 加 `Fs`、`nodes` 换、属主不在名单 → migrate、删除 → drain | B2 | B |
| C 启动与跟进 | C1 清单模式启动 | `daemon.cpp` 从清单建表；空表引导；`catalog.<node>` 登记；一致性校验改口径 | A1–A3 | C |
| | C2 轮询 + auto / manual 应用 | tick 轮询；`apply_catalog(v)` 流水线；`ctl cluster catalog apply` / `reload` / SIGHUP | C1 B3（或桩） | C |
| | C3 状态与指标 | `cluster status` 的 `catalog=` 字段；`lightnfs_cluster_catalog_*` | C2 | C |
| D 管理命令 | D1 ctl 引号 + `cluster catalog show/status/history/diff/import/rollback/apply` | 线协议引号；读类命令；`import` / `rollback` 的 CAS 写 | A2 A3 C1 | D |
| | D2 `cluster export list/add/set/remove` | 单导出增删改；护栏 | D1 | D |
| | D3 离线 `lightnfs-ctl catalog …` | 不经网关直接读写 `--shared-dir`（引导 / 救援） | A2 A3 | D |
| E 验收与文档 | E1 三实例脚本"清单"段 + `v4catalog` 验收模式 | 引导 → 在线加导出 → referral 可见 → 改 `nodes` 迁移 → 删除 STALE | C2 D2 B3 | E |
| | E2 文档 | 08 册、deployment.md §6、10 册、09 §9.3、design/README；followups | 全部 | E |

关键路径：A2 → A3 → C1 → C2 → D1 → D2 → E1；B1 → B2 → B3 → C2（真换版）。

---

## 阶段 A：清单文档与存储（无行为变化）

### A1 配置键 ✅ 2026-09-08

**目标**：解析并校验 11 §11.2 的三个新键；`exports_source = "local"`（默认）时后两者被忽略。

**改动点**

- `src/core/config.hpp`：`ClusterConfig`（`config.hpp:130`）新增

  ```cpp
  std::string exports_source = "local";   // local | catalog
  std::string catalog_refresh = "auto";   // auto | manual（仅 catalog）
  ```

  `Config`（`config.hpp:161`）新增 `std::map<std::string, backend::BackendConfig> backend_defaults;`
  （键 = 后端名）。
- `src/core/config.cpp` `parse_config`：`[cluster]` 段分发（`config.cpp:328`）加两键；新增段
  `[backend_defaults.<name>]`（仿 `[export.<name>]` 的子表分支 `config.cpp:206` / `:424`），只接受
  `kPerNodeBackendKeys`（`config.hpp:177`）里的键，其他键 EINVAL 并提示"put it in the catalog"。
- `validate_config`（`config.cpp:534`）：`exports_source` 取值；`catalog` 时 `cluster.enabled` 必须为
  真、本地 `exports` 必须为空（EINVAL："exports come from the catalog"）、`exports.empty()` 的
  既有拒绝（`config.cpp:536` 附近）放开；`local` 时出现 `[backend_defaults]` 只告警。
- `restart_required_report` / `cluster_restart_required_report`（`daemon.cpp:287` / `:306`）：
  `exports_source` 变化 → restart required；`catalog_refresh` 可热改（C2 读的是当前值）。

**测试**：`tests/test_ctl.cpp` 仿 `Ctl.ActiveActiveConfigKeys`（`test_ctl.cpp:321`）加
`Ctl.CatalogConfigKeys`：默认 local；catalog 下本地 `[[export]]` 被拒、空导出放行；
`[backend_defaults.cephfs]` 里放 `fs_name` 被拒、放 `conf` 通过；`catalog_refresh = bogus` 被拒。

**验收**：`lightnfsd --check-config` 对 11 §11.2 的本地示例返回 0；10 册示例结果不变。

**实现注**（2026-09-08）：`Config::backend_defaults` 为 `map<backend, BackendConfig>`；`[backend_defaults.<name>]`
的键在**解析期**就按 `kPerNodeBackendKeys` 过滤（`per_node_backend_key()`），值解析与 `[export.<name>]`
共用 `backend_value`；`cluster_catalog_exports()` 供 daemon / C1 判定。`run_server` 在 C1 落地前对
`exports_source = "catalog"` 直接拒绝启动（`--check-config` 仍通过），避免跑一台空导出表的网关。
`cluster_restart_required_report` 比较前把 `catalog_refresh` 抹平，其余 `[cluster]` 键（含
`exports_source`）仍是 restart required。测试 `Ctl.CatalogConfigKeys`（`tests/test_ctl.cpp`）。

### A2 `core/catalog.*`：清单文档 ✅ 2026-09-09

**目标**：清单的解析、序列化、与本地合并、集群级校验、diff，全部纯函数，不碰共享目录。

**改动点**

- 新文件 `src/core/catalog.hpp/.cpp`：

  ```cpp
  struct CatalogMeta { uint64_t version = 0; std::string updated_at, updated_by, comment; };
  struct CatalogExport {            // = ExportConfig 去掉本机键，加 disabled
    ExportConfig cfg;               // backend_config.values 里不含 kPerNodeBackendKeys
    bool disabled = false;
  };
  struct Catalog { CatalogMeta meta; std::vector<CatalogExport> exports; };  // 按 fsid 有序

  Result<Catalog> parse_catalog(std::string_view toml);   // [catalog] 头 + [[export]] 段
  std::string serialize_catalog(const Catalog&);          // 规范格式：键序固定、fsid 升序
  // 清单 → 本机 Config.exports：清单键优先，本机键只来自 backend_defaults[backend]
  Result<std::vector<ExportConfig>> merge_with_local(const Catalog&, const Config& local);
  // 集群级校验：fsid/path 唯一且不互为前缀、nodes 语法与去重、后端类型存在（find_backend）、
  // 同卷同进退（复用 validate_active_active 的分组逻辑，抽成 same_volume_groups）、
  // 本机键不得出现、disabled 导出仍占 fsid
  Result<void> validate_catalog(const Catalog&, bool active_active);
  struct CatalogDiff { std::vector<uint32_t> added, removed, disabled, enabled;
                       std::vector<uint32_t> nodes_changed, dynamic_changed, rejected; };
  CatalogDiff diff_catalog(const Catalog& from, const Catalog& to);   // rejected = path/backend/集群键变了
  // 从本地 Config 剥出清单（import 与 §11.9 迁移用）：去本机键、version 由调用方填
  Catalog catalog_from_config(const Config&);
  ```

- `parse_config` 的 `[[export]]` / `[export.<name>]` 分支抽成 `parse_export_sections(...)`
  （`config.cpp:194-206`、`:390-427`），两处共用；未知导出键从"静默忽略"改为 EINVAL（今天
  `:390-423` 无 else 分支，本册顺手收紧——只影响拼错的键）。
- `canonical_exports_text`（`config.cpp:614`）不改：合并后的 `ExportConfig` 走同一摘要，本地模式
  与清单模式对同一内容得到同一摘要（11 §11.9）。

**测试**：新 `tests/test_catalog.cpp`：往返（parse → serialize → parse 相等）；本机键出现被拒；
合并把 `[backend_defaults.cephfs] conf` 注入每个 cephfs 导出、不注入 local 导出；`validate_catalog`
的每条规则各一例（fsid 重复、path 互为前缀、`nodes` 重复、同卷不同 `nodes`、未知后端）；
`diff_catalog` 覆盖七类变化；`catalog_from_config` 去掉本机键且 `canonical_exports_text` 相等。

**实现注**（2026-09-09）：TOML 子集的值解析器与导出块解析器抽到内部头 `src/core/config_parse.hpp`
（`detail::` 命名空间，仅 `src/core` 内使用）：`ExportBlockParser` 逐行吃一个 `[[export]]` 块（键 +
`[export.<backend>]` 子表），`parse_config` 与 `parse_catalog` 共用，未知导出键一律 EINVAL（原先静默
忽略）；`disabled` 只在给了槽位（清单）时接受，本地文件里出现即 EINVAL；`[export.<backend>]` 子表必须紧跟其
`[[export]]` 块（中间隔了别的段再出现，原先会挂到上一个导出上，现在 EINVAL）。清单文件**一键一行**（08 册示例
里 `a = 1; b = 2` 的分号写法只是文档缩写，解析器不认）。`parse_catalog` 要求 `[catalog]` 头与 `version`
键，导出按 fsid 稳定排序，子表里的本机键告警丢弃（§11.2 的向前兼容）；`validate_catalog` 对内存里构造的
清单拒绝本机键，并多一个可选 `std::string* why` 出参（D1/D3 的错误文案），不给时走 WARN 日志。
`serialize_catalog` 写全部标量键（含默认值）、`nodes = []`、子表键排序，字符串转义与 `string_value`
互逆，子表值仅在与 `backend_value` 逐字互逆时才裸写（`"007"` 仍带引号）。`merge_with_local` 跳过
`disabled` 导出，结果按 fsid 升序。`diff_catalog` 的 `rejected` 独占，其余四类按字段独立（一个 fsid
可同时在 `enabled` 与 `nodes_changed`）。同卷同进退抽成 `same_volume_groups` +
`check_same_volume_nodes`，`validate_active_active` 改为复用；`nodes` 语法抽成 `valid_export_nodes`。
`ExportConfig` / `BackendConfig` 加默认 `operator==`。测试 `tests/test_catalog.cpp`（8 例）。

### A3 `ClusterStore` 清单键 ✅ 2026-09-09

**目标**：11 §11.3 的四个文件，**只加不改**。

**改动点**

- `src/server/cluster_store.hpp`（接口 `cluster_store.hpp:68-132`）新增：

  ```cpp
  struct CatalogDoc { uint64_t version = 0; std::string text; };       // version 从 [catalog] 头解出
  struct CatalogApplied { std::string node; uint64_t version; std::string digest;
                          int64_t applied_at_ms; std::string status; };  // "ok" | "error:<text>"
  virtual Result<std::optional<CatalogDoc>> read_catalog() = 0;          // 不存在 → nullopt
  // CAS：当前版本 != expected → EAGAIN；写前把当前文件复制到 catalog.history/<version>.toml
  virtual Result<uint64_t> write_catalog(uint64_t expected, std::string_view text) = 0;
  virtual Result<std::vector<uint64_t>> list_catalog_history() = 0;
  virtual Result<CatalogDoc> read_catalog_history(uint64_t version) = 0;
  virtual Result<void> put_catalog_applied(const CatalogApplied&) = 0;   // catalog.<node>
  virtual Result<std::vector<CatalogApplied>> list_catalog_applied() = 0;
  ```

- `PosixClusterStore`（`cluster_store.cpp:62-521`）：`write_catalog` 在 `lock("catalog")`
  （`cluster_store.cpp:475` 的 O_EXCL 锁，陈旧回收）下：读当前 → 比版本 → 复制到 history →
  `atomic_write_file`（`core/atomic_file.hpp:20`）→ 修剪 history 到 32 份。`read_catalog` 用
  `read_file_if_exists` 后只解 `[catalog] version`（不整份解析，轮询要便宜）。
  `list_exports_digests`（`cluster_store.cpp:141-157`）的 `exports.` 前缀过滤与新文件名无交集，
  加一条测试钉住。
- `tests/mem_cluster_store.hpp`：同名内存实现，`catalog_docs` / `catalog_applied` 两个 map，
  `fail_write_catalog` 注入。

**测试**：`tests/test_cluster_store.cpp` 加 `ClusterStore.CatalogCasAndHistory`（首写 expected=0；
版本错 → EAGAIN；history 内容 = 上一版；修剪；损坏的头 → EINVAL）、`CatalogAppliedPerNode`
（覆盖写、列出、不被当作 `exports.` 摘要——扩 `:566-570` 的断言）。

**实现注**（2026-09-09）：`write_catalog(expected, text)` **不改写文档**：`text` 的 `[catalog] version`
必须等于 `expected + 1`（否则 EINVAL），存储侧只做 CAS（当前版本 ≠ `expected` → EAGAIN；当前文件头损坏
→ EINVAL，先修复再提交）；调用方（D1/D3）用 A2 的 `serialize_catalog` 填好版本再写。轮询用的头解析是
`core::peek_catalog_version`（A2 文件里新增，只扫 `[catalog]` 段的 `version` 键，不解析导出）。历史保留
份数是 `server::kCatalogHistoryKeep = 32`，修剪在锁内尽力而为。`catalog.<node>` 的 status 是行尾剩余部分
（可含空格），digest 不得含空格；节点名 `toml` / `lock` / `history` 与清单自身文件重名，`put_catalog_applied`
拒绝（EINVAL），列出时也跳过。`MemClusterStore`：`catalog_docs`（version → text，最大者为当前版）、
`catalog_applied`、`fail_write_catalog` 注入。测试 `ClusterStore.CatalogCasAndHistory` /
`CatalogAppliedPerNode` / `MemCatalogMirrorsPosix`，`Catalog.PeekVersion`。

---

## 阶段 B：运行期可变导出集

### B1 `ExportSet` 快照 + RCU 发布（无行为变化） ✅ 2026-09-09

**目标**：把"导出表 + 伪根"变成不可变快照，读者每请求取一次；这一步**永不换版**，行为与今天
逐字节相同。

**改动点**

- `src/core/config.hpp` `ExportTable`（`config.hpp:229`）：

  ```cpp
  struct ExportSet {                       // 不可变
    uint64_t generation = 0;               // 集版本（B2 起每次换版 +1）
    std::vector<std::shared_ptr<ExportEntry>> entries;   // fsid 升序
    std::unique_ptr<const PseudoFs> pseudo;              // 由 entries 建
    const ExportEntry* by_fsid(uint32_t) const;          // 二分
    const ExportEntry* for_mount_path(std::string_view, std::string& rel) const;
  };
  class ExportTable {
   public:
    std::shared_ptr<const ExportSet> snapshot() const;   // atomic load
    // 既有 by_fsid / for_mount_path / entries() 保留为 snapshot() 上的便捷转发（过渡期），
    // 逐个调用点改成显式持快照后删除
  };
  ```

  `entries_` 改为 `std::atomic<std::shared_ptr<const ExportSet>>`；`build()` / `add()` 只在发布前
  操作一个可变的构建器（`ExportSetBuilder`）。`PseudoFs` 的构造从 `ProtocolStack`
  （`protocol_stack.cpp:57`）移到 `ExportSetBuilder::finish(epoch)`；`PseudoFs` 持
  `const ExportEntry*` 不变（条目由集持有）。
- 读者改成显式持快照（每处一个 `auto set = exports.snapshot();`，生命周期 = 一次请求 / 一次
  枚举）：`FileHandleCodec`（`core/file_handle.hpp:34/62/68`，解码 `file_handle.cpp:154/191`）、
  `nfsv3::Engine`（`nfsv3/engine.hpp:68`）、`nfsv4::Engine::resolve`（`nfsv4/engine.hpp:203`）、
  `mountd::Mount3` EXPORT 枚举（`mount3.cpp:88`）、`MutateGuard`（`core/mutate.hpp:56/84`）、
  指标遍历（`metrics_providers.cpp:138-160`）、`DataPlane::exports`（`ctl.hpp:41`）与
  `CoreState::exports`（`protocol_stack.hpp:39`）。v4 引擎的 `resolve()` 把快照挂在请求上下文里，
  一个 COMPOUND 内不换版。
- `FsClusterController::Fs::exp`（`cluster_controller.hpp:257`）暂不动（B3 改）。

**测试**：全部既有测试原样通过是主要门槛；`tests/test_config.cpp`（或新 `test_export_set.cpp`）
加 `ExportSet.SnapshotIsStable`（快照取到后 `by_fsid` 结果不随后续发布变化——B2 之前用
`ExportTable` 的测试钩子 `publish_for_test`）。基准 `tools/bench/bench_fullpath.cpp:66` 改用
构建器；`fuzz/fuzz_file_handle.cpp:25`、`fuzz/fuzz_handle_request.cpp:96` 同。

**验收**：`scripts/accept_m6_local.sh` 与 `bench fullpath` 吞吐无回退（快照取用是一次 atomic
load + shared_ptr 拷贝）。

**实现注**（2026-09-09）：`ExportSet{generation, epoch, entries, pseudo}` 与 `ExportSetBuilder`
（`add(cfg, backend)` / `finish(epoch, generation)`，可从既有集构建以共享条目）进 `core/config.hpp`；
`ExportTable` 只剩 `std::atomic<std::shared_ptr<const ExportSet>>`，`snapshot()` / `set_epoch()` /
`publish_for_test()`，`entries()` 删除（`size()` 顶替），`by_fsid()` 保留为启动代码与测试的便捷转发。
与草案的差异：`ExportSet::by_fsid` / `for_mount_path` 返回**非 const** `ExportEntry*`——集的成员关系冻结，
但条目自身的运行态（指标、QoS 桶、clients 指针）本来就是可变的，v3/v4 的 `Resolved::exp`、`PseudoFs::Node::exp`
都沿用非 const 指针；`check_client` / `squash_cred` 不依赖表状态，改成 `ExportTable` 的静态函数，
`MutateGuard` 不再收表，`FileHandleCodec` 不再绑定表（`from_key(key)` 单参，`decode` / `decode_v4`
显式收 `const ExportSet&`）。`entries` 按 fsid 升序（`by_fsid` 二分），因此 MOUNT EXPORT 枚举、
`/metrics` 的 export 行、`fdcache` 等 ctl 列表从配置顺序变为 fsid 顺序——这是唯一可见的顺序差异，协议语义
不变。`generation` 每次发布 +1（`add` / `set_epoch` / `publish_for_test` 都算）。伪根的 change 基值：
`ExportTable::build` 先以 epoch=1 发布，`ProtocolStack` 构造时 `set_epoch(core.epoch)` 重发一版
（主备重激活换 epoch 时同样生效）；`PseudoFs` 构造改收 `entries` 向量，`root()` 改 const。持快照的
粒度：v4 `Ctx::set`（一个 COMPOUND 一版，`resolved` 里的伪根节点指针随之有效）；v3 `Resolved::set`
（一次 resolve 一版，rename/link 两次 resolve 各持一份，都不失效）；MNT / EXPORT 每次调用一版；
`StateMgr` 的 native-lock 钩子每次调用一版；指标 / ctl / daemon 启停各自一版。
`FsClusterController::Fs::exp` 仍取启动集（B3 改）。测试 `tests/test_export_set.cpp`（7 例：空表伪根、
`SnapshotIsStable`、fsid 排序与跨版共享、构建器拒绝项、`for_mount_path` 最长前缀、`set_epoch` 重建伪根、
`reload_dynamic` 就地更新不发布）；bench / fuzz 夹具改用构建器。验收：`lnfs_tests`（332 例）+ 三个 accept 脚本
（Release + ASAN）合并前后原样通过；`bench fullpath 1 4 20000 32` GETATTR 基线 347–361k rps → 353–387k rps，
READ4k 289–312k → 315–322k rps（同机连跑三次，无回退）。

### B2 换版：新增 / 就地更新 / 退休 ✅ 2026-09-09

**目标**：`ExportTable::apply(plan)` 发布新集，条目跨版共享，删除的条目退休后 `stop()`。

**改动点**

- `core/config.hpp`：

  ```cpp
  struct ExportSetPlan {                       // 由 diff_catalog + merge_with_local 生成
    std::vector<ExportConfig> add;             // 新 fsid（含 enabled 回来的）
    std::vector<ExportConfig> update;          // 同 fsid：clients/qos/readonly/squash/anon/nodes
    std::vector<uint32_t> remove;              // 删除 / disabled
  };
  // 主循环线程调。add 的后端由调用方先 make + start()（run_on_reactor(0)）再传入。
  std::shared_ptr<const ExportSet> apply(ExportSetPlan, std::vector<std::unique_ptr<backend::Backend>> started, uint64_t epoch);
  // 退休队列：apply 后被移出集的条目；tick 上调，use_count()==1 的条目 stop() 并释放
  std::vector<std::shared_ptr<ExportEntry>> take_retired();
  ```

- `ExportEntry`：`readonly` / `squash` / `anon_uid` / `anon_gid` 改成 `std::atomic`（读点都是每请求
  读一次标量：`squash_cred` `config.cpp`、引擎的 ROFS 判定）；`nodes` 改成与 `clients_` 相同的
  原子指针 + 退休列表（`config.hpp:198-205` 的写法），控制器 tick 线程读它。
- 伪根：`ExportSetBuilder::finish` 用 `epoch << 32 | generation` 做伪根目录的 change 属性基值
  （`pseudofs.cpp` 目前用启动 epoch），保证换版后递增。
- `reload_dynamic`（`config.cpp:728`）改为在当前快照的条目上就地更新（语义不变），供本地模式
  继续用。

**测试**：`ExportSet.ApplyAddsRemovesAndSharesEntries`（新增 fsid 出现在新集与伪根；未变的条目
是同一指针；删除的条目在旧快照释放前 `take_retired()` 为空、释放后返回它）；
`ExportSet.PseudoChangeMonotonic`；`ExportSet.DynamicFieldsUpdateInPlace`（readonly 翻转后同一条目
的读值变化）。

**实现注**（2026-09-09）：`ExportTable::apply(plan, started&, epoch)` 返回
`Result<std::shared_ptr<const ExportSet>>`，`started` 改为**引用**：成功时被消费（清空），失败时原样留给
调用方（草案按值传入会让已 `start()` 的后端随错误一起丢失，无人 `stop()`）。校验先于任何改动：后端数与
`add` 不等、`add` 的 fsid 为 0 / 重复 / 已在集中（同一 plan 里 remove + add 同 fsid 视为**替换**，允许）、
`update` / `remove` 指向未知 fsid、`update` 与 `remove` 同 fsid、`update` 改了 path、clients CIDR 不合法
→ 全部 EINVAL，不发布、不动条目。通过后：新集共享未删除的条目（`ExportSetBuilder::keep`），`add` 的条目带入
已启动后端，`update` 在共享条目上就地改 clients / qos / readonly / squash / anon / nodes（老快照上的读者
同样看到），然后发布（generation +1），被删条目进退休队列（记录入队时刻）。`take_retired()` 只交出
`use_count()==1` 的条目（所有列过它的快照都已释放），其余留队；另有 `retired_pending()` /
`oldest_retired()` 供 C2 做 10 × lease 告警。队列有互斥锁保护，虽然 apply / take 都约定在主循环。
`ExportEntry`：`readonly` / `squash` / `anon_uid` / `anon_gid` 改 `std::atomic`（读点隐式 load，原有
`if (exp->readonly)` 写法不变）；`nodes` 改为 `node_list()` / `set_nodes()`（与 `clients_` 同一套原子指针
+ 退休列表），默认构造即发布空列表，裸构造的 `ExportEntry` 也能读。伪根 change 基值改为
`ExportSet::pseudo_change() = epoch << 32 | generation`，`ExportSetBuilder::finish` 用它建
`PseudoFs`，每次发布（含 `set_epoch`、`add`、就地更新的 apply）都单调递增，重启后 `(epoch+1) << 32`
仍大于上一代任何版本；B1 里 `PseudoFs` 的 change = 启动 epoch 的断言随之改掉。`reload_dynamic` 已在 B1
改成快照上就地更新，本步只把 `nodes` 比较换成 `node_list()`。`FsClusterController::Fs::exp` 仍是启动集的
裸指针，在 B3 改成跟随集之前**不得**对多活网关调用 `apply` 删除导出（C2 接入时以 B3 为前置）。测试：
`tests/test_export_set.cpp` 新增 4 例（上述三例 + `ApplyRejectsBadPlansWithoutPublishing`），
`tests/test_nfs4.cpp` 加 `Nfs4.ExportSetSwapUnderRunningEngine`（引擎运行中加导出 → 下一 COMPOUND
可见；删导出 → 旧 fh STALE、伪根名消失、快照释放后条目退休）。

### B3 控制器 `sync_exports` ✅ 2026-09-09

**目标**：11 §11.7。

**改动点**

- `server/cluster_controller.hpp`：`FsClusterController` 新增
  `void sync_exports(const std::shared_ptr<const core::ExportSet>&)`（主循环线程调，持 `mu_`）；
  `Fs::exp` 改为 `std::shared_ptr<const core::ExportEntry>`（`cluster_controller.hpp:257`）；构造
  函数从 `exports.snapshot()` 建 `fs_`（`cluster_controller.cpp:341`）。
- 实现：新 fsid → `fs_[fsid] = Fs{entry, kStandby}`；`nodes` 变 → 换 `exp`，若 `role == kActive` 且
  `node_` 不在新 `nodes` 且有活着的候选 → `request_migrate(fsid, 首个活着的)`（复用 `:756`），无
  候选 → 每 tick 告警一次；fsid 消失 → Active/Activating → `begin_draining(fsid, "removed from
  catalog", fence_lost=false, release=true)`，Standby → 直接 erase；drain 完成（`run_draining`
  `:693-707`）后 erase。`publish()` 随之。
- failover 的 `ClusterController` 不需要 per-fsid 逻辑；其 `activate` 重建协议栈时自然用最新集。

**测试**：`tests/test_cluster_controller.cpp` 加 `FsClusterController.SyncExportsAddsAndTakes`
（新 fsid 下一 tick 被 `nodes[0]` 接管）、`SyncExportsNodesChangeMigratesOwner`（属主不在新名单
→ owner 记录指向名单首个活着的、本机 drain）、`SyncExportsRemovalDrainsOwner`（Active 的被删
fsid → deactivate 钩子被调、围栏释放、`fs_` 里消失）、`SyncExportsNodesChangeNoCandidateKeeps`。

**实现注**（2026-09-09）：`Fs::exp` 改为 `std::shared_ptr<const ExportEntry>`，并缓存上次 sync 时的
`nodes` 副本（`Fs::nodes`）——B2 的 `apply` 对同 fsid 是**就地**改 `node_list()`，条目指针不变，所以
"nodes 变了"只能靠比较内容发现。控制器持有被删条目的引用直到 drain 完成，因此 `ExportTable::take_retired()`
（`use_count()==1`）不会在状态还没丢干净时把后端交出去 `stop()`；C2 的退休 tick 天然排在 drain 之后。
`sync_exports(set)`：新 fsid → `Fs{entry, kStandby}`，视图 Unowned，下一 tick 按 `our_turn` 接管（sync
本身不接管）；`nodes` 变 → 换 `exp`、更新缓存，若 `role == kActive` 且本机不在新名单 → `migrate_off`
按名单顺序逐个 `request_migrate`（它自己校验目标已注册且心跳活着，EHOSTDOWN 就试下一个），全都不行
→ WARN 并继续服务；fsid 消失 → Standby 直接 erase（顺手 `release_fs_fence` 清掉可能残留的过期 hold），
Active / Activating → 标 `removed` 并 `begin_draining("removed from catalog", release=true)`，已在
Draining 的只标 `removed`；`run_draining` 看到 `removed` 就 erase 而不是回到 Standby，并且**无论**
`release` 参数如何都释放本机的 hold（没人会再续这个 fsid，留着会挡住它日后回归）。sync 时给该导出标 `Fs::evict`
（Activating 也标，等它完成），`tick()` 每轮对 "Active 且 `evict`" 的导出重试 `migrate_off`，失败则每
tick 一条 WARN；只看 `evict` 而不是重新比较名单，是为了保住 `cluster takeover --force` 让名单外节点接管
后"留在原地"的既有行为——这样 sync 时无候选、
之后候选上线的情形也能接手（`SyncExportsNodesChangeNoCandidateKeeps` 钉住）。顺带修了一处时序：
`run_activation` 在跑 takeover / activate 钩子**之前**先检查角色仍是 Activating（原先钩子跑完才检查，
"投递后被 drain / 删除"会白跑一次 activate 再 deactivate）。`snapshot()` 的 `nodes` 仍取
`exp->node_list()`。B3 只加控制器能力，没有生产调用方；`daemon.cpp` 在 C2 把 `apply` → `sync_exports`
→ 退休 tick 串到主循环上。测试 4 例如上，另在删除用例里覆盖了"Activating 中被删"（激活让路、drain、
erase）与退休队列配合。

---

## 阶段 C：启动与跟进

### C1 清单模式启动 ✅ 2026-09-09

**目标**：11 §11.4 的启动流程与空表引导。

**改动点**

- `server/daemon.cpp` `run_server`（`daemon.cpp:432-653`）：`exports_source == "catalog"` 时在
  `make_posix_cluster_store`（`:444`）之后、`build_core_state`（`:451`）之前：`read_catalog()` →
  有 → `parse_catalog` + `validate_catalog` + `merge_with_local` → 填 `config->exports` → 既有路径
  （`ExportTable::build`、`check_cluster_backends` `:453`、`start_backends` `:484`）；无 → 空表
  启动并 `LNFS_WARN("catalog: none yet; serving no exports until one is published")`。
  记录 `applied_catalog_version`；`put_catalog_applied({node, v, digest, now, "ok"})`。
- `check_exports_consistency`（`:185-207`）：清单模式下不 refuse，只 `list_catalog_applied()` 打印
  同伴版本并对落后 / 领先者告警；`put_exports_digest` 照写。
- `build_core_state`（`:132`）保留 `Config` 的本地部分（`backend_defaults`、`cluster`）供 C2 合并
  用——今天 `Config` 在 `:451` 被 move 进 `build` 后丢弃，改为 `core.local_config` 持有本机部分。
- 空导出表：`ProtocolStack` / `PseudoFs` / `Mount3` 对零导出的路径加测试（伪根只有 `/`）。

**测试**：`tests/test_daemon_lifecycle.cpp` 加 `CatalogBootFromStore`（内存 store 里放 v3 清单 →
表有其导出、`catalog.<node>` = 3 ok）、`CatalogBootEmpty`（无清单 → 零导出、伪根可 PUTROOTFH）、
`CatalogBootLocalMergeFails`（缺 `[backend_defaults.cephfs]` → 启动 EINVAL 指出 fsid 与键）。

**实现注**（2026-09-09）：启动逻辑抽成新文件 `src/server/catalog_boot.hpp/.cpp`（只依赖 `ClusterStore`
接口，内存 store 可测）：`load_catalog_exports(store, local&, why*)` = `read_catalog` → `parse_catalog`
→ 头版本与存储版本一致性 → `validate_catalog`（按本机 `mode` 决定 nodes 是否必填）→ 本机
`[backend_defaults.<b>]` 只含本机键（否则 EINVAL，文案带 catalog 版本、fsid、path、键名）→ `merge_with_local`
→ 填 `local.exports` 并置 `Config::exports_from_catalog`（新增字段：`validate_config` 在清单模式下拒绝的是
"本地文件里的 `[[export]]`"，合并来的导出要放行，否则 `ExportTable::build` 的再校验会拒）→ `validate_config`；
`validate_config` 不报是哪个导出，失败时逐个导出单独校验一遍定位（`blame_export`），文案 "catalog vN:
export fsid=X (path): ENOENT" 之类；跨导出规则（同卷同进退）定位不到就只报 errno。无清单 → WARN 并返回
`present=false`，`local.exports` 空。与草案的差异：本仓库没有任何后端把本机键设为**必填**（cephfs 缺 conf
用库默认、gluster 同理），所以"缺 `[backend_defaults.cephfs]`"只 WARN 一行不报错，`CatalogBootLocalMergeFails`
钉住的是三种真实失败：本机 defaults 混入集群键（解析器在文件里就拒，测试用内存构造）、本机不存在的导出
路径（指出 fsid 与路径）、清单本身不过集群级校验，外加解析失败与 store 读错误（errno 透传，不是 EINVAL）。
`record_catalog_applied(store, node, v, digest, status)` 写 `catalog.<node>`，失败只 WARN；
`check_catalog_consistency` 列出同伴的 `catalog.<node>`：同版 ok 记 INFO，落后 / 领先 / error 记 WARN，
从不拒绝；`exports.<node>` 摘要照写（本地模式同伴的 09 §9.3 校验用）。`daemon.cpp`：`run_server` 在建
store 之后、算摘要之前调 `load_catalog_exports`，`check_exports_consistency` 在清单模式下换成
`check_catalog_consistency`，后端全部 `start()` 成功后写 `catalog.<node> = v ok`（无清单写 `0 ok`，让
同伴看得到这台在清单模式）；`cluster mode:` 日志尾部加 `catalog=vN|none`。`CoreState` 新增 `local_config`
（去掉 exports 的本机 Config，供 C2 合并）、`catalog_exports`、`applied_catalog_version`（供 C2/C3）。
`--check-config` 在清单模式下也建 store 读清单（只读，`PosixClusterStore` 构造与 `read_catalog` 不写共享
目录）并打印 `catalog: vN` / `none yet`。`reload`（ctl / SIGHUP）在清单模式下跳过 `reload_dynamic`，报告
一行"exports come from the catalog"，避免把清单导出全报成 "removed from config"；真正的应用在 C2。空表
路径：`Mount3.EmptyExportSetServesNothing`（MNT ACCES、EXPORT 空、旧句柄 STALE）、
`Nfs4.EmptyExportSetHasABarePseudoRoot`（PUTROOTFH、READDIR 空、LOOKUP NOENT），`CatalogBootEmpty`
还用 `activate()` 起了一个零导出数据面并经 ctl 看到 `exports=0`。手工冒烟：真实 `lightnfsd` 在
`shared_dir` 放 v1 清单 → `--check-config` 打印 `catalog: v1`，起服务 `status` 报 `exports=1`，
`catalog.gw1` = `1 <digest> <ms> ok`；删掉清单再起 → `exports=0`、`catalog.gw1` = `0 … ok`。

### C2 轮询 + auto / manual 应用 ✅ 2026-09-09

**目标**：11 §11.4 的跟进流水线。

**改动点**

- `server/cluster_controller.cpp` `tick()`（`:459-563`）末尾：`store_.read_catalog()`（只解版本）；
  `> applied_` → `catalog_refresh == "auto"` ? `hooks_.post(apply_catalog(v))` : `pending_ = v`。
  同一 tick 只投递一次；应用中不重复投递（`applying_` 标志）。failover 的 `ClusterController`
  围栏线程同样处理。
- 新 `server/catalog_applier.{hpp,cpp}`（主循环线程）：

  ```cpp
  class CatalogApplier {  // 持 ClusterStore&, ExportTable&, local Config, FsClusterController*, runtime
    Result<uint64_t> apply_latest();   // 读 → 解析 → 校验 → merge → diff → plan → 后端 make+start →
                                       // ExportTable::apply → sync_exports → put_catalog_applied
    uint64_t applied() const; uint64_t pending() const; std::string last_error() const;
  };
  ```

  B 未落地时的桩：`diff.added/removed` 非空 → 返回 `EBUSY` 并报 `restart required: fsid=…`，
  其余（`nodes` / dynamic）就地应用——与 `reload_dynamic` 的口径一致；B3 合并后换成真换版。
- 触发点统一：`CtlDeps::reload`（`ctl.hpp:92`，今天在 ctl reactor 直接跑 `do_reload`
  `daemon.cpp:492`）改为投递到主循环并等待结果（`MainLoop::post` + promise），顺带修掉
  "ctl reactor 上做阻塞文件 IO"；`do_reload` 在清单模式下末尾调 `applier.apply_latest()`；
  SIGHUP（`daemon.cpp:631`）同。新 ctl `cluster catalog apply` 只调 `apply_latest()`。
- 退休队列：`ExportTable::take_retired()` 在主循环上由 tick 投递的 `retire_exports()` 处理，
  `stop()` 走 `run_on_reactor(0)`。

**测试**：`tests/test_cluster_controller.cpp` 加 `CatalogAutoApplyOnTick`（store 里版本 +1 → post
被调一次 → 应用后 `catalog.<node>` 更新）、`CatalogManualLeavesPending`（manual → `pending()` = v、
不 post；`apply_latest()` 后清零）、`CatalogApplyFailureKeepsOld`（合并失败 → 旧集不变、
`catalog.<node>` = old error:…、`last_error()`）。`tests/test_ctl.cpp` 加 `reload` 经主循环的断言
（`AnswerCommandSurface` `:518` 的 reload 钩子改为主循环投递后行为不变）。

**实现注**（2026-09-09）：B 已落地，直接是真换版，没有桩。新文件 `src/server/catalog_applier.hpp/.cpp`：
`CatalogApplier{Deps{store, exports, local(CoreState::local_config), node, fs_cluster*, post,
start_backend, stop_backend, retire_overdue}}`，构造时收启动的 `CatalogBoot`（版本、文档、摘要——
`CatalogBoot` 为此加了 `catalog` 字段）。轮询没放进控制器内部，而是两个控制器的 `Hooks` 各加
`after_tick`（tick 线程、每 tick 末尾、不论 tick 做了什么都调），`daemon.cpp` 把它接到 `applier->poll()`；
`poll()` 只 `read_catalog`（A3 的头解析）比版本：更新 → auto 就 `post(apply_latest)`（`applying_` 标志保证
同时只有一份在投递 / 运行），manual 只记 `pending_`；退休队列非空时另投递一次 `retire_exports()`
（`take_retired` → `stop_backend` 走 reactor 0；仍被持有且超过 `retire_overdue` = 10 × lease 的每周期
WARN 一次）。`apply_latest()`（主循环）= 读最新 → 解析 → 头版本校验 → `validate_catalog` → 本机
`merge_with_local` + `validate_config`（失败按导出定位，同 C1）→ `diff_catalog(已应用文档, 新文档)`：
`rejected` 非空整版拒绝（EINVAL，文案 "fsid N changed path / backend / cluster keys: remove and
re-add"）；added / enabled → `plan.add`，removed / disabled → `plan.remove`，nodes / dynamic 变化 →
`plan.update`——再与**表里实际有的**对账（表里已有的 add 降为 update、表里没有的 remove 跳过、表里有
而合并结果没有的补 remove、合并结果有而表里没有的补 add），所以重试与部分失败后的再应用都是幂等的
→ 新增导出 `find_backend` / `make` / `start_backend`，任何一步失败把**已启动**的停掉、不发布 →
`ExportTable::apply` → `fs_cluster->sync_exports` → 写 `catalog.<node> = v ok`。失败：旧集不动、
`catalog.<node> = <old> error:<why>`（摘要仍是运行中的）、`failures()` +1、`last_error()`；同一失败版本
auto 模式下每 `kRetryEveryPolls`（30）个 poll 才自动重试一次（避免每 tick 刷一条 ERROR），手动
`apply` 不受限，新版本出现即刻重试。清单从 store 消失不算错（无事可做）。`apply_now()` 投递到主循环并
等结果（ctl 用，超时 ETIMEDOUT）。**主循环**：`daemon.cpp` 里的 `MainLoop` 抽到
`src/server/main_loop.hpp`，加 `call(fn, timeout)`（投递 + `promise` 等待；在主循环线程上调用则
直接执行，避免自等）和 `on_loop_thread()`，`run()` 改收停止 / SIGHUP 两个谓词（信号全局量留在
`daemon.cpp`）。ctl `reload` 现在是 `loop.call(reload_inline, 60s)`，文件 IO 与清单应用都离开了 ctl
reactor；SIGHUP 在主循环线程上直接调 `reload_inline`；两者在清单模式下末尾都 `apply_latest()` 并把
"catalog: vN applied (was vM) / is current / apply failed" 附在报告末尾；`reload_config` 在清单模式下
跳过 `reload_dynamic`，改为热更新 `local_config.cluster.catalog_refresh`（A1 说好的唯一可热改的
`[cluster]` 键）。新 ctl `cluster catalog apply`（文本 / `--json`；未启用清单模式答
"catalog: not enabled"；失败答 "catalog apply failed (still vN): <why>"），经 `CtlDeps::catalog`
（`Management::start` 多一个参数）。退出路径：`take_down` 之后调一次 `retire_exports()`，把数据面
消失后才无人引用的被删导出的后端也停掉。测试：计划三例 + `CatalogAutoApplyOnTick` 里的删除 → drain
→ 退休 → `stop` 与就地更新（同一条目、无后端启停）、`ClusterController.CatalogPollAfterTick`（主备
控制器也调钩子，store 读错也调）、`Ctl.ClusterCatalogApply`（不启用 / 已是最新 / 应用 / `--json` /
失败文案）、`DaemonLifecycle.MainLoopCallRunsOnTheLoopThread`（在主循环线程执行、嵌套调用内联、
SIGHUP 谓词、未运行时超时后 `drain` 补跑）。真机冒烟（`fence_lease = "1s"`，auto）：写 v2 加导出 →
2.5 s 内 `exports=2`、`catalog.gw1 = 2 … ok`；v3 删导出 → `exports=1`，日志 "retired: backend
stopped"；`reload` 报 "catalog: v3 is current"；v4 含本机不存在的路径 → 仍 `exports=1`，
`catalog.gw1 = 3 … error:catalog v4: … fsid=9 … ENOENT`，`cluster catalog apply` 同样文案；manual
模式下写 v6 不动，`kill -HUP` 后 `exports=2`，日志 "reload (SIGHUP): …;catalog: v6 applied (was v5)"。
`fence_lease = "300ms"` 这类毫秒写法解析器不收（与本步无关，记在这里）。

### C3 状态与指标 ✅ 2026-09-10

- `cluster_fs_status`（`ctl.cpp:235`）网关行加 `catalog=<applied|none> catalog_latest=<seen|none>
  catalog_refresh=auto|manual catalog_error=-|<text>`；JSON 同名字段；failover 的 `cluster_status`
  （`:177`）同。
- `FsClusterController::append_metrics`（`cluster_controller.cpp:922`）加
  `lightnfs_cluster_catalog_version`、`lightnfs_cluster_catalog_latest_version`、
  `lightnfs_cluster_catalog_pending`（0/1）、`lightnfs_cluster_catalog_applies_total`、
  `lightnfs_cluster_catalog_apply_failures_total`；failover 控制器同一组。
- 测试：`tests/test_ctl.cpp` `ClusterFsCommands`（`:685`）与 `tests/test_metrics.cpp`
  `ClusterFsSeries`（`:124`）各加断言。

**实现注**（2026-09-10）：指标没放进两个控制器的 `append_metrics`——控制器不认识应用器，应用器又在
控制器之后才构造——而是 `CatalogApplier` 构造时自己 `obs::register_text_provider`、析构时注销（与控制器
同一套注册表纪律：注销返回后不会再有 scrape 停在 provider 里），主备与多活自然是同一组：
`lightnfs_cluster_catalog_version`（已应用，无清单为 0）、`_latest_version`（store 里最近一次看到的
版本：启动、poll 或 apply 读到的；清单从 store 消失为 0）、`_pending`（0/1）、`_applies_total`
（换版成功次数）、`_apply_failures_total`。应用器为此新增 `latest_` / `applies_` 和一把锁下的
`status()` 快照（`Status{applied, latest, pending, applies, failures, last_error, refresh}`，`refresh`
即 poll 读到的 `catalog_refresh` 热值），ctl 与指标都只取它。`cluster status`：两种控制器的网关行
末尾（主备在 `last_activation_ms=` 之后，多活在 `exports=` 之后）追加 `catalog=<v|none>
catalog_latest=<v|none> catalog_refresh=auto|manual catalog_error=-|<text>`，`catalog_error` 放最后
因为是自由文本（含空格与冒号，解析器取 `catalog_error=` 之后整段）；JSON 同名字段，无版本 / 无错误为
`null`。**本地模式（`deps.catalog` 为空）一个字节不变**——四个字段只在清单模式出现，既有的精确断言原样
通过。测试：`Ctl.ClusterFsCommands` / `Ctl.ClusterCommands` 末尾各加一段（无清单启动 → poll 看到新版
只动 `catalog_latest` → 应用失败文案上行 → 清单消失回 `none` 而错误仍在；主备用一份解析不了的 v4）；
`Metrics.CatalogSeries` 单独一例而不是塞进 `ClusterFsSeries`（后者的导出是内存后端加假路径，跑不了真实
apply），钉住五个系列在启动 / 待应用 / 应用 / 失败 / 清单消失五个时刻的值，以及 provider 随应用器注销；
控制器三例补 `latest()` / `applies()` / `status()` 断言。08 册 §8.6 与指标表、deployment.md §6 的观测段
同步加了这四个字段与五个系列。

---

## 阶段 D：管理命令

### D1 ctl 引号与 `cluster catalog …` ✅ 2026-09-10

**改动点**

- `server/ctl.cpp` `parse_command`（`:67-77`）：支持 `"…"`（内部 `\"`、`\\`），空白切分在引号外；
  `--json` 仍可在任意位置。`tools/lightnfs_ctl.cpp` `run_cluster_cmd`（`:99-107`）对含空白 / 引号 /
  反斜杠的参数自动加引号。
- `ctl.cpp` 多活分支（`:724`）与 failover 分支都接 `cluster catalog <sub>`（清单与 mode 无关，
  抽成 `cluster_catalog_cmd(deps, cmd, json)`，两处调）：
  - `show`：`read_catalog` → `parse_catalog` → 头一行 `version= updated_at= updated_by= exports=` +
    每导出 `fsid= path= backend= nodes= disabled= clients= readonly= squash= …`；
  - `status`：`list_catalog_applied()` × `alive_peers()` → 每网关 `node= applied= status= alive=`，
    末行 `latest=`；
  - `history`：版本 / 时间 / 更新者 / comment；
  - `diff [v1] [v2]`：`diff_catalog` 的七类各一行；
  - `import <file> [--dry-run] [--comment …]`：`load_config(file)` 或直接 `parse_catalog`（文件可以
    是本地 TOML 或清单 TOML）→ `catalog_from_config` → `validate_catalog` → `write_catalog(expected
    = 当前版本)`，`EAGAIN` 重试 3 次；`--dry-run` 只报 diff；
  - `rollback <version>`：`read_catalog_history(v)` → 以新版本提交；
  - `apply`：C2 的 `apply_latest()`。
  所有共享目录 IO 走 `rt::offload`（既有模式 `:687` 等）；`apply` 投递主循环。
  `updated_by` = `node + " uid=" + SO_PEERCRED`（`CtlServer` 的 accept 处取一次）。
- `tools/lightnfs_ctl.cpp` `make_cluster_leaf`（`:110`）：帮助文本与 `--dry-run` / `--comment` 标志；
  `CtlDeps` 加 `CatalogApplier*` 与 `ClusterStore*`。

**测试**：`tests/test_ctl.cpp` 加 `Ctl.ParseCommandQuotes`、`Ctl.ClusterCatalogCommands`（内存
store：无清单时 `show` 答 `catalog: none`；`import` 建 v1 并写 history；`status` 列出 applied；
并发写模拟：store 版本被改 → EAGAIN 重试成功；`rollback` 产生 v3 内容 = v1；`--json` 同形）。

**实现注**（2026-09-10）：线协议的解析器抽成公开的 `parse_ctl_command` → `CtlCommand{args, json, error}`
（`ctl.hpp`，单元测试直接打）：引号外按空白切分，引号内 `\"` / `\\` 还原为字符，其余反斜杠原样；**裸**
`--json` 才是渲染开关，`"--json"` 是普通参数；引号在词中间也接（`x"y z"w` → `xy zw`）；未闭合引号置
`error`，`answer` / `answer_async` 两种渲染都答 "unterminated quote"。客户端 `wire_arg`：含空白 / 引号 /
反斜杠或为空的参数加引号并转义；`normalize_argv` 顺手把 `--comment TEXT` 折成 `--comment=TEXT`（ccmd 只收
等号形）。命令本体在新文件 `src/server/ctl_catalog.hpp/.cpp`（`cluster_catalog_answer(deps, cmd, peer_uid)`，
全部同步阻塞），`answer_async` 把整个命令丢进一次 `rt::offload`——store / 文件 IO 与 `apply` 等主循环都不在
reactor 上；原来 ctl.cpp 里的 `apply` 分支搬了过去，文案不变。清单命令只依赖 `CtlDeps::store`（新增
`ClusterStore*`，`Management::start` 多一个参数，daemon 传 `cluster_store.get()`），所以**本地模式也能用**
（§11.9 第 1 步：先 `import` 再切换）；无 store（单网关）答 `cluster: not enabled`；有 store 无应用器时只有
`apply` 答 "catalog apply: not enabled (exports_source = \"local\")"。`updated_by` = `<node> uid=<SO_PEERCRED>`
（`serve()` 已取的 ucred 经 `answer_async` 新的可选参数传入；node 取自任一控制器，测试里无控制器为 `?`）；
`updated_at` 为 UTC ISO-8601 秒。与草案的差异：(1) `status` 行的字段序是 `node= applied= alive= applied_at=
digest= status=`——`status` 是自由文本（`error:<why>` 含空格）所以放行尾；`alive` 多活取 `alive_peers()`，主备
只知道围栏持有者（其余 `?`）；末行 `latest=`。(2) `show` 头行 `version= exports= updated_at= updated_by=
comment=`，每导出一行按草案字段外加 `keys=`（集群级后端键 `k=v,…`），JSON 里导出数组叫 `exports_list`
（`exports` 是计数）。(3) `history` 把当前版也列出来并标 `current=yes|no`，解析不了的版本标 `unparseable`。
(4) `diff` 的版本可写 `0` / `none`（首发前的空清单）；没给两个版本时 `from` = 本网关已应用版（无应用器则报
"give the two versions to compare"）。(5) `import` 按 §11.5 拒绝 `rejected` 非空（同 fsid 改 path / backend /
集群键），文案与应用器一致；文件是清单文档（有 `[catalog]` 头）就直接 `parse_catalog`（头由本次提交重填），
否则 `parse_config` → `catalog_from_config`；`validate_catalog` 的 `active_active` 取 `deps.fs_cluster != null`；
CAS 失败（EAGAIN）重读重试，最多 `kCatalogCommitRetries` = 3 次后报 "another writer keeps changing the
catalog"；`--comment` 既收 `--comment <text>` 也收 `--comment=<text>`。(6) `rollback` 拒绝当前版（"is the
current catalog"），默认 comment "rollback to vN"，也收 `--comment`。测试：`Ctl.ParseCommandQuotes`、
`Ctl.ClusterCatalogCommands`（无 store / 无清单的每个命令 → dry-run 不写 → v1 含审计头 → `show` 文本与 JSON →
`status` 两台记录 → v2 → `history` / `diff 1 2` / `diff none 2` / 版本不存在 → 改 path 与重复 fsid 被拒 → 清单文档
导入 → 并发一次 EAGAIN 后 v5（`MemClusterStore` 新增 `before_write_catalog` 钩子）→ 永远有人抢先 3 次后报错 →
`rollback 1` 内容 = v1、拒绝当前版与未知版 → `apply` 无应用器文案），`Ctl.ClusterCatalogApply` 改为先无 store
再有 store。真机冒烟（单网关清单模式、`fence_lease = "1s"` auto）：`lightnfs-ctl cluster catalog import <file>
--comment "hello world, first"`，导出路径含空格，`catalog.toml` 的 `updated_by = "gw1 uid=1000"`、comment 完整；
`show` / `status` / `history` / `diff` / `cluster exports` 看到带空格的路径；v2 删导出 → 2.5 s 内 `exports=1`、
"retired: backend stopped"；`rollback 1` → v3 内容 = v1、`exports=2`；`diff 1 3` 全空；`apply` 答 "v3 already
applied"。

### D2 `cluster export list/add/set/remove` ✅ 2026-09-11

**改动点**

- `ctl.cpp`：`cluster export <sub>`（同样与 mode 无关）：
  - `add`：标志解析（`--path --fsid --backend --nodes a,b --clients c1,c2 --readonly --squash
    root|all|none --anon-uid --anon-gid --read-bps --write-bps --iops --opt k=v（可重复）
    --disabled --comment`）→ `CatalogExport` → 追加 → `validate_catalog`（含 fsid 复用规则：历史
    32 版内同 fsid 但 path/backend/集群键不同 → 拒绝，`--force` 覆盖）→ CAS 写；
  - `set <fsid>`：只接受在线可变标志；`--path` / `--backend` / `--opt` → `EINVAL "remove and re-add,
    or use a new fsid"`；
  - `remove <fsid> [--force]`：按本网关 `FsOwnerView` 的属主视图（多活）/ 本机是否 active
    （failover）判断"正在被服务" → 无 `--force` 拒绝；
  - `list` = `catalog show` 的导出部分。
- 服务端做的是**集群级**校验；本机 `[backend_defaults]` 合并留给各网关应用时（11 §11.8）。

**测试**：`Ctl.ClusterExportCommands`：`add` 后 v+1 且 `show` 可见；重复 fsid / path 前缀 / 本机键
`--opt conf=…` / 未知后端各被拒；`set --nodes` 改顺位；`set --path` 被拒；`remove` 被属主护栏
拒、`--force` 通过；fsid 复用规则。

**实现注**（2026-09-11）：命令与 D1 同在 `src/server/ctl_catalog.cpp`（`cluster_export_answer`，`answer_async`
同样整体 `rt::offload`）。**提交路径改成"变更函数"**：D1 的 `commit(next)` 在 CAS 重试时会把基于旧版算出的
整份文档写上去、覆盖掉抢先者的改动（对 `import` / `rollback` 这种整份替换无所谓，对单导出增删改是丢更新），
现在 `commit(mutate)` 每次尝试都重读当前版再施加变更（`Mutation = f(current) → next`），`import` / `rollback`
用 `replace_with(doc)`；`validate_catalog` 与 §11.5 的 `rejected` 检查也搬进循环、对每次的 `next` 做。测试
用 `before_write_catalog` 钩子让别人先提交 fsid 9，我们的 `add fsid 4` 重试后 v12 里两者都在。标志解析
`parse_export_flags`：`--k=v` 与 `--k v` 都收；`--readonly` / `--disabled` / `--force` / `--dry-run` 裸写为 true、
`=true|false` 明示（`set` 靠这个清除）；`--opt k=v` 可重复；未知标志、多余位置参数、缺值都是明确文案。
`add`：`--path` / `--fsid` 必填，其余默认同 `ExportConfig`（`clients` 默认全开，`backend` 默认 local）；同
fsid 已在清单 → "already in the catalog (…): use set, or remove it first"；集群级规则（重复 / 嵌套 path、本机键、
未知后端、坏 clients、多活缺 `nodes`）全由 `validate_catalog` 报 "add failed: invalid catalog: <why>"；**fsid
复用规则**（`check_fsid_reuse`）：从新到旧扫历史，最近一个含该 fsid 的版本若 path / backend / 集群键不同
（用 `diff_catalog` 的 `rejected` 判定）→ EEXIST "fsid N was <path> (<backend>) in vK: … (--force to reuse it
anyway)"，同身份放行。`set <fsid>`：`--path` / `--backend` / `--opt` → "path, backend and backend keys cannot
change: remove and re-add, or use a new fsid"；没给任何可变标志 → "nothing to change"；fsid 不在清单 → ENOENT。
`remove <fsid>`：护栏 `served_by`——多活看控制器快照（view 的属主，或本机 activating / active），主备看本机
`role() == Active`（活动网关服务清单里的一切）——非空且无 `--force` → "fsid N is served by gw2 (disable it
first, or --force)"；`remove` 只收 `--force` / `--dry-run` / `--comment`。`list` = `show` 的导出行（无清单
"catalog: none"，空清单 "no exports (catalog vN)"），JSON `{"version", "exports_list"}`。三个写命令的回答统一
`catalog vN committed (was vM): export fsid=X added|updated|removed: <diff 摘要>`，`--dry-run` 为 "would be
…" 且不写；JSON `{"version","was","fsid","action",<diff 七类>}`。**客户端**：`cluster export …` 的每导出标志
太多、且 `set` 需要区分"没给"与 `=false`，不走 ccmd——`main` 里 `normalize_argv` 后发现 `cluster export` 就
`run_cluster_export_raw`：除 `--socket` / `-s` / `--json` 外逐个 `wire_arg` 原样转发，服务端解析并回 usage。
测试 `Ctl.ClusterExportCommands`（多活控制器 + 内存 store：无 store / 无清单 / usage → 各种坏标志文案 →
dry-run 不写 → 多活缺 nodes 被拒 → 全标志 `add` 与 `list` 文本 / JSON → 重复 fsid / 嵌套 path / 本机键 / 未知
后端 / 坏 clients 被拒 → `set` 改顺位 + 清 readonly、`--disabled`、身份字段被拒、无变化、未知 fsid → 护栏（本机
与对端属主）、`--force`、无人服务的直接删 → fsid 复用同身份放行 / 异身份拒 / `--force` → 并发提交不丢 →
主备控制器 Active 时护栏、`--force`）。真机冒烟（单网关多活清单模式）：`lightnfs-ctl cluster export add --path
"…/my dir" --fsid 1 --nodes gw1 --readonly --comment "first export, spaced"`、`add --path=… --opt handles=auto`
→ 2.5 s 内 `exports=2`；`set 1 --readonly=false --iops 50` 就地生效（日志 "1 updated"）；`set 1 --path` 被拒；
`remove 2` 被护栏拒、`--force` 后 `exports=1`；同 fsid 换路径被 fsid 复用规则拒；`--bogus` 与无子命令各回文案。

### D3 离线 `lightnfs-ctl catalog … --shared-dir <dir>`

**目标**：不经网关直接操作共享目录（首次引导、所有网关都起不来时的救援）。

**改动点**：`tools/lightnfs_ctl.cpp` 加根级 `catalog <show|import|history|rollback>` 叶子，链接
`lnfs_core` + `PosixClusterStore`（`make_posix_cluster_store`，`cluster_store.hpp:140`），复用 D1 的
纯函数；`updated_by = "offline uid=<getuid>"`。`import` 支持 `--from-local <lightnfs.toml>`
（`catalog_from_config`）。

**测试**：`tests/test_ctl.cpp` 里对纯函数的覆盖已足够；离线路径在 E1 脚本里用于引导。

---

## 阶段 E：验收与文档

### E1 三实例脚本"清单"段 + `v4catalog` 验收模式

**改动点**

- `scripts/accept_active_active_local.sh`：`write_config`（`:53-120`）加 `LNFS_EXPORTS=catalog` 分支：
  本地文件无 `[[export]]`、`exports_source = "catalog"`；启动前用 D3 的离线 `catalog import
  --from-local` 引导 v1（三台 `nodes` 顺位与今天相同）。新段 `catalog`：
  1. 三台 `cluster catalog status` 都在 v1、既有四段（status / v4moved / roll / crash）原样通过；
  2. 对 gw1 发 `cluster export add --path … --fsid 3 --nodes gw3,gw1`；等 gw3 `cluster exports gw3`
     含 fsid 3；`lnfs_accept_client v4catalog` 对 gw1：伪根 READDIR 出现新导出、进入即
     MOVED + `fs_locations` 指向 gw3、change 属性比加之前大；
  3. `export set 3 --nodes gw2,gw1`：等 gw2 接管（属主 gw3 不在名单 → migrate），客户端在 gw3 收
     `LEASE_MOVED`；
  4. `export set 3 --disabled=true`：所有网关 `role=` 行消失、客户端对旧句柄收 STALE；
     `export remove 3` 通过；`--force` 路径单独一遍；
  5. `catalog_refresh = manual` 的一台：`status` 显示 `pending`、`cluster catalog apply` 后追平；
  6. `catalog rollback 1` 后全部回到 v1 的导出集。
  Release 与 ASAN 各一轮；`level=error` 为零。
- `tests/accept_client.cpp` 加 `v4catalog` 模式（伪根 READDIR 断言 + change 单调 + STALE）。

### E2 文档

- 08 册：`[cluster] exports_source / catalog_refresh`、`[backend_defaults]`、`cluster catalog|export`
  命令、`lightnfs_cluster_catalog_*` 指标、热重载口径（清单模式下 readonly/squash/anon 在线）。
- `deployment.md` §6：加"清单模式"小节与 §11.9 的迁移步骤；`cluster_roll.sh` 说明不变。
- 10 册：§10.10 指向 11 册；§10.3 键空间表加 `catalog.*`；09 §9.3 摘要说明加清单模式口径。
- `design/README.md`：11、12 条目；11 册状态改"已实现"；本册撤下，未闭环项进
  `docs/toto/shared-export-catalog-followups.md`。

## 12.2 每步的通用验收清单

- [ ] `exports_source = "local"`（默认）下：`lnfs_tests`、三个 accept 脚本、`bench fullpath` 结果与
      合并前一致。
- [ ] 新增代码 clang-format / clang-tidy 无新告警（全仓库既有漂移不在本册范围）。
- [ ] 共享目录新文件名不与 `exports.` 前缀过滤、`fence.` / `epoch.` 命名冲突（A3 测试钉住）。
- [ ] 主循环线程纪律：后端 `start()`/`stop()`、`ExportTable::apply`、`sync_exports` 只在主循环 /
      reactor 0 上跑；tick 线程只读共享目录与投递。
- [ ] 每个 ctl 新命令有文本与 `--json` 两种断言，错误路径有明确文案。
