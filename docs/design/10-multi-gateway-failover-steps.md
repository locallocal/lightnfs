# 10. 多网关无感故障切换——实现步骤拆分

> 状态：**实施计划**。本册把 [09 册](09-multi-gateway-failover.md) 的方案拆成可独立合并、
> 可独立验证的步骤；每步给出改动点（带现有代码锚点）、接口形态、测试与验收标准。
> 09 册回答"做什么、为什么"，本册只回答"按什么顺序、改哪里、怎么证明做对了"。

## 10.0 总原则

1. **默认零行为变化**：所有改动挂在 `[cluster] enabled`（默认 `false`）之后。单网关路径
   （现在的 `run_server`）在每一步合并后都必须与合并前逐字节等价——用现有 ctest +
   `scripts/accept_m6_local.sh` 作为回归门。
2. **一步一个 PR，可单独回滚**：步骤之间只允许向前依赖（见 §10.1 依赖图），不允许一个 PR
   横跨两个阶段。
3. **先抽象后替换**：先把"本机 state_dir"访问抽成接口并让本机实现通过原有测试
   （阶段 A），再接入共享目录实现（阶段 B/C）。这样每一步的 diff 都是"只改一处"。
4. **每步自带测试**：单元测试进 `tests/`，跨进程场景进 `lnfs_accept_client` + `scripts/`；
   没有测试的步骤不合并。

## 10.1 阶段与依赖

| 阶段 | 步骤 | 交付物 | 依赖 |
|------|------|--------|------|
| A 基础设施（无行为变化） | A1 `[cluster]` 配置段 ✅ 2026-09-05 | 解析、校验、restart-required 报告 | — |
| | A2 `ClusterStore` 接口 + POSIX 实现 ✅ 2026-09-05 | 原子写、epoch、名单、围栏、导出摘要 | — |
| | A3 稳定状态访问抽象 ✅ 2026-09-05 | 句柄密钥 / epoch / 名单三处改走接口，本机实现保持原语义 | A2 |
| | A4 管理面与数据面分离 ✅ 2026-09-05 | ctl/metrics 不再随 `Frontend` 生死 | — |
| B 协议与状态语义 | B1 集群身份 ✅ 2026-09-05 | server_owner/scope 从 cluster id 派生 | A1 |
| | B2 reclaim 下推失败 → DELAY | 状态层 grace 内重试 | — |
| | B3 CLAIM_DELEG_PREV_FH | 接受为普通 open 状态 | — |
| | B4 导出表一致性摘要 | 启动期拒绝树不一致的网关入集群 | A1 A2 |
| C 进程生命周期 | C1 `ProtocolStack` 可重建 | 栈的构造/析构与进程解耦；连接收敛等待 | A4 |
| | C2 `ClusterController` 状态机 | Standby/Activating/Active/Draining + 围栏协程 | A1–A4 B1 C1 |
| | C3 ctl `cluster *` 命令 | status / takeover / standby | C2 |
| | C4 指标 | role / epoch / fence age / takeovers | C2 |
| D 后端接管钩子 | D1 `Backend::takeover()` 接口 + 脚本钩子 | 默认空操作；可配外部脚本（Lustre evict 等） | C2 |
| | D2 CephFS reclaim 原语 | cephapi 三入口、fake、`[export.cephfs] uuid` | D1 |
| E 验证与文档 | E1 `v4failover` 验收模式 + 本机双实例脚本 | 无 root 的端到端证明 | C3 D1 |
| | E2 fake 注入与脑裂演练 | 残留锁、围栏分离 | B2 C2 D2 |
| | E3 文档 | 08 册、deployment.md、07 §7.5、09 册状态更新 | 全部 |

关键路径：A2 → A3 → C1 → C2 → E1。阶段 B 的四步与阶段 A 并行无冲突。

---

## 阶段 A：基础设施

### A1 `[cluster]` 配置段（已完成，2026-09-05）

**目标**：解析并校验 09 §9.3 的配置段；`enabled=false` 时所有字段被忽略。

**改动点**

- `src/core/config.hpp`：`ServerConfig` 旁新增

  ```cpp
  struct ClusterConfig {
    bool enabled = false;
    std::string id;            // 非空；建议 UUID，只校验 [A-Za-z0-9_-]{8,64}
    std::string shared_dir;    // 绝对路径
    std::string node;          // 默认 gethostname()；exports.<node> 与围栏记录用
    std::string role = "auto"; // active | standby | auto
    uint32_t fence_lease_ms = 3000;   // 续租周期；失效 = 3× 未续
    std::string takeover = "auto";    // auto | manual
    std::string takeover_hook;        // 可选外部脚本（D1）
    bool unsafe_skip_backend_checks = false;  // 仅测试：跳过 kStableHandles/kByteLocks 校验
  };
  ```

  挂在 `Config::cluster`。
- `src/core/config.cpp`：`Section` 枚举加 `kCluster`（`src/core/config.cpp:183` 段名分发处）；
  `fence_lease` 复用 `[protocol] lease/grace` 的时长解析（`src/core/config.cpp:312`），
  允许 `ms`/`s` 后缀，范围 `[500ms, 60s]`。
- `validate_config`（`src/core/config.cpp:405`）：`enabled` 时要求 `id`、`shared_dir` 非空且
  `shared_dir` 为绝对路径；`role`/`takeover` 取值合法；**`server_owner`/`server_scope` 不得
  显式设置**（集群模式由 `id` 派生，显式值会让两台网关身份分叉，直接 EINVAL）。
  `shared_dir` 是否可写留到运行期（`--check-config` 只 `access(W_OK)` 并告警，不写入）。
- 后端能力校验放在 `server/daemon.cpp` 的 `build_core_state` 之后（能力位只有构造出
  backend 才能读到）：每个导出必须 `caps().has(kStableHandles) && caps().has(kByteLocks) &&
  native_locks().has_value()`，否则启动失败并逐条打印缺什么；
  `unsafe_skip_backend_checks=true` 时降级为 WARN（供 E1 的本机 local 后端测试使用）。
- `restart_required_report`（`src/server/daemon.cpp:157`）：`[cluster]` 任一字段变化 →
  `"cluster settings changed: restart required"`。

**测试**：`tests/test_ctl.cpp` 仿 `Ctl.OpsConfigKeys` 加 `Ctl.ClusterConfigKeys`——默认值、
时长后缀、非法 role、显式 server_owner 被拒、`enabled=false` 时坏值被忽略。

**验收**：`lightnfsd --check-config` 对 09 §9.3 示例配置返回 0；单网关配置不受影响。

### A2 `ClusterStore` 接口与 POSIX 实现（已完成，2026-09-05）

**目标**：09 §9.4 的共享目录访问层，含原子写与串行化。

**改动点**

- 先抽公共原子写：把 `core::bump_boot_epoch`（`src/core/boot_epoch.cpp:13`）里的
  "tmp + write + fsync + rename + fsync(dir)" 提成 `core::atomic_write_file(path, bytes)`
  （新文件 `src/core/atomic_file.{hpp,cpp}`），`bump_boot_epoch` 改为其调用者。
- 新文件 `src/server/cluster_store.{hpp,cpp}`：

  ```cpp
  struct FenceRecord { std::string node; uint64_t epoch; int64_t expires_at_ms; };

  class ClusterStore {
   public:
    virtual ~ClusterStore() = default;
    virtual Result<std::array<std::byte, 16>> load_or_create_key() = 0;
    virtual Result<uint64_t> read_epoch() = 0;
    virtual Result<uint64_t> bump_epoch() = 0;                 // 串行化：epoch.lock
    virtual Result<std::vector<std::string>> list_clients() = 0;  // co_ownerid 原文
    virtual Result<void> put_client(std::string_view owner_id) = 0;
    virtual Result<void> erase_client(std::string_view owner_id) = 0;
    virtual Result<std::optional<FenceRecord>> read_fence() = 0;
    // 仅当无记录 / 已过期 / force 时改写为 {node, epoch, now+ttl}；否则 EBUSY
    virtual Result<FenceRecord> acquire_fence(std::string_view node, uint64_t epoch,
                                              std::chrono::milliseconds ttl, bool force) = 0;
    virtual Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) = 0;  // 不是自己的 → EPERM
    virtual Result<void> release_fence(std::string_view node) = 0;
    virtual Result<void> put_exports_digest(std::string_view node, std::string_view digest) = 0;
    virtual Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() = 0;
  };
  std::unique_ptr<ClusterStore> make_posix_cluster_store(std::string shared_dir);
  ```

- POSIX 实现细节：
  - 目录布局与 09 §9.4 一致；`clients/` 文件名沿用 `state_mgr.cpp:228` 的 `fnv64(owner)` 十六进制，内容为原文。
  - `epoch` 与 `fence` 的写入先取 `<name>.lock`（`O_CREAT|O_EXCL`，内容 `node pid unix_ms`）；
    锁文件超过 `2 × fence_lease` 未释放视为持有者已死，`unlink` 后重试一次。
  - 时间用墙钟毫秒（`CLOCK_REALTIME`），过期判定加 `500ms` 偏差容忍；文档要求各网关 NTP 同步。
  - 一切写路径走 `atomic_write_file`；读路径容忍文件暂缺（返回空/nullopt 而非错误）。

**测试**：新文件 `tests/test_cluster_store.cpp`（临时目录）——密钥首建后复用且 0600；
两个 store 对象交替 `bump_epoch` 严格单调；A 取围栏后 B `acquire` 得 EBUSY，把过期时间
改到过去后 B 成功、A `renew` 得 EPERM；`force` 无视过期；遗留锁文件被回收；
`put/erase/list_clients` 往返；导出摘要多节点列表。

**验收**：`atomic_file` 重构后 `StateMgr.GraceListPersistsAndEarlyExit` 等既有测试不变。

### A3 稳定状态访问抽象（本机实现保持原语义）（已完成，2026-09-05）

**目标**：把三处"直接读写 state_dir"改为经接口，本机实现下行为不变，为 C2 切换到
`ClusterStore` 铺路。

**改动点**

- 句柄密钥：`core::load_or_create_hmac_key(path)` 已在 A2 抽出（`ClusterStore` 复用它）；集群模式下
  `daemon` 改用 `ClusterStore::load_or_create_key()` 再 `FileHandleCodec::from_key`
  （已有，`src/core/file_handle.hpp:21`）。
- 名单：`StateMgr::Config` 新增

  ```cpp
  struct StableStore {  // 空 = 用 state_dir/clients/（现状）
    std::function<std::vector<std::string>()> load;
    std::function<void(std::string_view owner)> put;
    std::function<void(std::string_view owner)> erase;
  } stable;
  ```

  `load_grace_list`（`state_mgr.cpp:158`）、`persist_client`（`:227`）、`unpersist_client`
  （`:246`）三处改为：有 `stable` 钩子则调钩子，否则走现有本机代码。写失败策略不变
  （只 WARN，`state_mgr.cpp:237`）。
- epoch：`CoreState::epoch` 的来源改为 `build_core_state` 的入参（`daemon.cpp` 的
  `local_identity` / `cluster_identity`）；单网关仍 `bump_boot_epoch`。集群模式的目标是
  此处**不 bump**（推迟到 C2 的 Activating）；**过渡期**（C2 之前）集群模式在进程启动时经
  `ClusterStore::bump_epoch()` 推进一次，否则重启后 epoch 不变、旧 stateid 不会被判 STALE。
  C2 把这次 bump 挪进 Activating 后删除该过渡逻辑。
- 集群模式下 `ProtocolStack` 已把 `StateMgr::Config::stable` 三钩子接到 `ClusterStore`
  （`protocol_stack.cpp` 的 `cluster_stable_store`），`CoreState::cluster` 为该 store 指针。

**测试**：`tests/test_state.cpp` 加 `StateMgr.StableStoreHooks`——内存 map 实现的三钩子；
确认 CREATE_SESSION 后 `put`、状态全清后 `erase`、新实例 `load` 后进入 grace。

### A4 管理面与数据面分离（已完成，2026-09-05）

**目标**：`ctl` 套接字与 metrics HTTP 在 Standby（没有协议栈、不监听）时也要可用，
否则 `lightnfs-ctl cluster takeover` 无处可发。

**改动点**

- `src/server/frontend.hpp`：把 `ctl`、`metrics` 从 `Frontend` 移到新的
  `struct Management { std::unique_ptr<CtlServer> ctl; std::unique_ptr<MetricsHttp> metrics; }`，
  由 `run_server` 在阶段 3（runtime 起来后）启动，最后一个停。
- `CtlDeps`（`src/server/ctl.hpp`）里指向数据面的成员（`drc`、`state`、`exports`、
  `drain`、`draining`）改为可切换：`std::shared_ptr<DataPlaneSlot>`（`std::atomic<const DataPlane*>`），
  `DataPlane` 是 `{exports, drc, state, drain, draining}` 的聚合；为空时相关命令回
  `"not active"`（JSON：`{"error":"not active"}`）。`status` 输出加 `role=`（无 `role` 钩子时
  由是否 attach 派生 active/standby；C2 的控制器通过 `CtlDeps::role` 提供细分状态）。
  `reload` 是进程级钩子，栈不存在时跳过 per-client QoS（`reload_config` 的 `stack` 可空）。
- `Frontend::start`（`src/server/frontend.cpp:73` 附近的 ctl 组装）改为只建监听器 + rpcbind，
  并调用 `Management::attach(DataPlane)`；`Frontend::stop` 调 `detach()`。

**测试**：`tests/test_ctl.cpp` 的 `AnswerCommandSurface` 加"未 attach 时 state/drc/drain
回 not active，ping/version/status 仍可用"。

**验收**：单网关模式下 ctl 行为、套接字路径、权限检查不变（`Ctl.FragmentedCommandFramingAndSocketPerms` 通过）。

---

## 阶段 B：协议与状态语义

### B1 集群身份（已完成，2026-09-05）

**改动点**：派生逻辑抽成 `server::derive_server_identity(cfg, cluster)`（`protocol_stack.hpp`），
`ProtocolStack::enable_v4` 多一个 `ClusterConfig` 入参：`cluster.enabled` 时
`derived = "lightnfs-cluster:" + cluster.id`，owner 与 scope 都用它，`minor_id` 仍为 0。
非集群路径保持 `hostname:state_dir`（显式 `server_owner/scope` 仍优先）。`nfsv4::Engine`
增加 `server_owner()/server_scope()` 只读访问器供测试与 C3 的 `cluster status` 使用。

**测试**：`Nfs4.ClusterIdentityDerivation`（纯函数：单机/显式/集群/不同集群/enabled=false）、
`Nfs4.ClusterIdentityAcrossProtocolStacks`（两个 `ProtocolStack`，不同 `state_dir` 与 epoch、
相同 cluster id → 相同 owner/scope；去掉集群段则不同）。`lnfs_accept_client` 的 v4 客户端
在 EXCHANGE_ID 后记录 `server_owner`，`v4walk` 打印它，供双实例脚本比对线上取值。

**顺带修复**：双实例同时在空 `shared_dir` 上启动时暴露出 `load_or_create_hmac_key` 的竞争——
读者可能在创建者 `O_EXCL` 建文件之后、写满 16 字节之前读到短文件（EINVAL）。改为写私有临时文件
后用 `link()`（无硬链接的文件系统退回 `renameat2(RENAME_NOREPLACE)`）原子发布，读者只会看到
"不存在"或"完整"两种状态；`atomic_write_file` 的临时名也改为进程内唯一
（`.tmp.<pid>.<n>`）。`ClusterStore.KeyConcurrentCreatorsAgree` 覆盖 8 线程 × 20 轮并发创建。

### B2 reclaim 模式下的下推失败 → DELAY

**目标**：09 §9.7 第 1 条。故障网关在存储侧残留的锁让新网关 grace 内的 LOCK(reclaim)
下推被拒时，客户端得到 DELAY 重试而不是 DENIED 丢锁。

**改动点**：`StateMgr::lock`（`src/state/state_mgr.cpp:1634` 的 `push_native_lock` 调用后）：

```cpp
if (pushed.status == as_u32(Status::kDenied) && args.reclaim && in_grace()) {
  native_lock_reclaim_delays_.fetch_add(1);
  pushed.status = as_u32(Status::kDelay);   // 回滚网关授予的逻辑保持不变
}
```

grace 结束后仍被拒 → 原样 DENIED（此时确实有第三方持锁）。新增计数器
`lightnfs_v4_native_lock_reclaim_delays_total`（`src/server/metrics_providers.cpp:51` 旁）。

**测试**：`tests/test_state.cpp` 仿 `NativeLockPushRollbackAndRelease`：fake `LockMgr` 前 N 次
`lock()` 回 EAGAIN——名单内客户端 grace 内 LOCK(reclaim) 得 DELAY，重试至 N 次后成功；
`end_grace()` 后同一场景得 DENIED。

### B3 CLAIM_DELEG_PREV_FH 接受为普通 open 状态

**目标**：09 §9.7 第 3 条，让 Linux 客户端找回委托的路径零退避。

**改动点**：`src/nfsv4/engine.cpp:1531` 的 `case kClaimDelegPrevFh`：从 NOTSUPP 分支移出，
按 RFC 8881 §18.16 该 claim 无附加参数；设 `is_reclaim = true`、`deleg_claim = false`，
走与 `kClaimPrevious` 相同的 `state_.open(args.reclaim=true)` 门禁（名单 + grace，否则
RECLAIM_BAD / NO_GRACE，`state_mgr.cpp:776`）；应答 `OPEN_DELEGATE_NONE`，不调用
`maybe_grant_read_deleg`。`kClaimDelegatePrev`（4.0 式带文件名）保持 NOTSUPP。
grace 期不授新委托已由 `maybe_grant_read_deleg` 的 `in_grace()` 判定覆盖
（`state_mgr.cpp:931`），无需改动。

**测试**：`tests/test_nfs4.cpp`——重启后名单内客户端 CLAIM_DELEG_PREV_FH → OK 且
`delegation_type == NONE`；未列名单 → RECLAIM_BAD；grace 外 → NO_GRACE。

### B4 导出表一致性摘要

**目标**：09 §9.3 末条：fsid 相同而树不同的网关不得入集群。

**改动点**

- `core::ExportTable::canonical_digest()`（`src/core/config.hpp`）：对每个导出取
  `path | fsid | backend | readonly | squash | anon_uid | anon_gid` 与后端子表中
  **除按节点豁免键之外**的所有 `key=value`（排序后拼接），整体做摘要。`util/` 里没有
  SHA-256，句柄用的 SipHash-2-4 只有 64 位：新加一个约 100 行的 SHA-256
  （`src/util/sha256.{hpp,cpp}`，带 FIPS 180-4 测试向量）。豁免键列表是常量
  `kPerNodeBackendKeys = {conf, keyring, id, user, name, log_file, fd_cache, mon_host}`，
  写在 `cluster_store.hpp` 并在 08 册记录。
- `daemon` 启动（集群模式，backend `start()` 之前）：`put_exports_digest(node, digest)`，
  然后 `list_exports_digests()` 逐一比对，任一不同 → 打印对方节点名与本机摘要，退出码 1。
  被移除节点的陈旧 `exports.<node>` 需要运维手动删除（deployment.md 说明；不做自动 GC）。

**测试**：`tests/test_cluster_store.cpp` 加摘要用例：仅 `keyring` 不同的两份配置摘要相同；
`subdir` 不同则不同。

---

## 阶段 C：进程生命周期

### C1 `ProtocolStack` 可重建 + 连接收敛等待

**目标**：09 §9.6 的前提——接管时重新构造协议栈（epoch 在构造时固化），退出服务时能
干净销毁而不停 runtime。

**改动点**

- `src/server/daemon.cpp` 的 `run_server`：`ProtocolStack stack(...)` 与 `Frontend` 改为
  `struct DataPlaneInstance { std::unique_ptr<ProtocolStack> stack; std::optional<Frontend> frontend; MetricsRegistration metrics; }`，
  由 `activate(core, runtime, epoch)` 构造、`deactivate()` 销毁。单网关模式：
  `run_server` 启动时调一次 `activate`，退出时 `deactivate`，顺序与现在的镜像关停一致。
- `register_metrics_providers`（`src/server/metrics_providers.cpp:185`）改为返回 RAII
  `MetricsRegistration`；`obs/metrics.hpp` 的 `register_text_provider` 增加返回句柄与
  `unregister_text_provider(handle)`（`src/obs/metrics.hpp:164`）。
- 租约扫描器：`ProtocolStack` 新增 `stop_lease_scanner()`——置 `lease_stop` 后等待协程
  实际退出（`run_lease_scanner` 结束时 signal 一个 `rt::Event`/latch），保证析构栈时没有
  协程还在引用 `state`。
- 连接收敛：`Frontend::stop` 目前只停 accept，"存量连接随 runtime 收敛"。加
  `transport::ConnRegistry::close_all()`（`ctl conns/kill-conn` 已依赖该注册表）与
  `wait_idle(timeout)`：Draining 时先 `drain`（停 accept）→ 等在途请求完成，上限
  `2 × fence_lease` → 超时后 `close_all()` → 等计数归零 → 才允许析构 `ProtocolStack`。

**测试**：`tests/test_transport_loopback.cpp` 加 `close_all + wait_idle`；新增
`tests/test_daemon_lifecycle.cpp`（不监听真实端口，用回环临时端口）：`activate → deactivate →
activate` 两轮，ASAN 下无泄漏、无 use-after-free；metrics 提供者数量两轮后不增长。

**验收**：`accept_m6_local.sh` 的 reclaim/courtesy/ctl 段全过（单网关路径等价）。

### C2 `ClusterController` 状态机与围栏协程

**目标**：09 §9.5/§9.6 的核心。

**改动点**

- 新文件 `src/server/cluster_controller.{hpp,cpp}`：

  ```cpp
  enum class Role { kStandby, kActivating, kActive, kDraining };
  class ClusterController {
   public:
    struct Hooks {
      std::function<Result<void>(uint64_t epoch)> activate;   // 建栈 + 监听（C1）
      std::function<void()> deactivate;                        // drain + 收敛 + 析构
      std::function<Result<void>()> backend_takeover;          // D1
      std::function<void()> backend_reset;                     // stop()+start() 全部后端
    };
    ClusterController(const core::ClusterConfig&, ClusterStore&, Hooks);
    rt::Task<void> run(rt::CancelToken);   // 围栏协程：Standby 轮询 / Active 续租
    Result<void> request_takeover(bool force);  // ctl（C3）
    Result<void> request_standby();             // ctl
    Snapshot snapshot() const;                  // role/epoch/fence/node，供 ctl 与指标
  };
  ```

- 转换规则：
  - **Standby**：每 `fence_lease` 读 `fence`。`role=active`，或 `role=auto && takeover=auto`
    且围栏缺失/过期 → Activating。`takeover=manual` 只响应 `request_takeover`。
  - **Activating**（目标 < 1s，09 §9.6）：① `acquire_fence(node, epoch+1, 3×lease, force)`
    → ② `bump_epoch()` → ③ `backend_takeover()`（失败只 WARN，B2 的 DELAY 路径兜底）
    → ④ `activate(epoch)`（其中 `StateMgr` 的 `stable.load` 读共享名单并进 grace）
    → Active。任一步硬失败 → `release_fence` → 回 Standby 并计数。
  - **Active**：每 `fence_lease` `renew_fence`；返回 EPERM（围栏被别人改写）或连续 3 次
    IO 失败 → Draining，原因写日志并计入 `fence_lost_total`。
  - **Draining**：`deactivate()` → `backend_reset()`（让 gluster 的 `glfs_fini` 等释放存储侧
    残留，09 §9.7 Gluster 条）→ `release_fence`（仅当仍是自己）→ Standby；若是进程退出
    路径则到此结束。
- 线程模型：围栏协程跑在最后一个 reactor（与租约扫描器同处，`protocol_stack.cpp:63`
  的选择）；`activate/deactivate` 涉及 `Frontend::start`（主线程语义），所以控制器不直接
  调用，而是把请求投递到主线程事件循环——`wait_for_shutdown_signal`
  （`src/server/daemon.cpp:205`）改成"条件变量 + 事件队列"的循环，统一处理信号、
  SIGHUP reload、activate/deactivate 请求。
- `run_server` 集群分支：阶段 2 用 `ClusterStore` 取密钥、不 bump epoch；阶段 3 起后端后
  启动 `Management`（A4）与控制器；不再无条件 `activate`。
- 单网关分支不构造控制器，与 C1 之后完全相同。

**测试**：`tests/test_cluster_controller.cpp`：用内存 `ClusterStore` fake + 记录调用序列的
Hooks——
- Standby 见围栏过期自动接管，Hooks 调用顺序 = fence → epoch → takeover → activate；
- Active 续租时围栏被改写 → deactivate → backend_reset → 不 release 他人围栏；
- `takeover=manual` 时围栏过期不动作，`request_takeover(force=true)` 覆盖未过期围栏；
- activate 失败 → 围栏释放、回 Standby、epoch 已 +1（允许，单调即可）。

### C3 ctl `cluster` 命令

**改动点**

- `src/server/ctl.cpp`：命令 `cluster status | cluster takeover [--force] | cluster standby`，
  `CtlDeps` 加 `ClusterController* cluster`（空 → `"cluster: not enabled"`）。
  `status` 文本：`role=active node=gw1 epoch=17 fence_owner=gw1 fence_age_ms=812 shared_dir=... peers=gw1,gw2`，
  `--json` 同字段。用法字符串（`src/server/ctl.cpp:93`）同步更新。
- `tools/lightnfs_ctl.cpp`：新增 `cluster` 子命令组（仿 `drc [flush]` 的带参叶子，
  `tools/lightnfs_ctl.cpp:194`）。

**测试**：`Ctl.AnswerCommandSurface` 加三条命令的文本/JSON 断言（fake 控制器）。

### C4 指标

**改动点**：控制器注册一个独立文本提供者（生命周期与进程相同，不随栈重建）：

```
lightnfs_cluster_role{role="standby|activating|active|draining"} 1   # 其余 role 为 0
lightnfs_cluster_epoch <n>
lightnfs_cluster_fence_age_seconds <f>          # 自己持有时为距上次续租；否则距他人记录
lightnfs_cluster_takeovers_total <n>
lightnfs_cluster_fence_lost_total <n>
lightnfs_cluster_activation_seconds{quantile=...}  # 或直方图，覆盖 §9.6 "< 1s" 目标
```

`lightnfs_v4_reclaims_total` 与 `lightnfs_v4_in_grace` 沿用。

**测试**：`tests/test_metrics.cpp` 加解析断言；`test_cluster_controller` 中接管一次后
`takeovers_total == 1`。

---

## 阶段 D：后端接管钩子

### D1 `Backend::takeover()` 接口 + 外部脚本钩子

**改动点**

- `src/backend/api.hpp` `class Backend`：

  ```cpp
  struct ClusterIdentity { std::string cluster_id, node; uint64_t epoch; };
  // Activating 第 3 步调用：尽快释放故障网关在存储侧的残留。默认空操作。
  virtual rt::Task<Result<void>> takeover(const ClusterIdentity&);
  ```

  默认实现在 `api.cpp`（`co_return {}`）。`kBackendApiVersion` 保持 1（纯新增虚函数，
  三个集群后端在同一仓库，无 ABI 兼容问题；05 册"可选扩展"记一笔）。
- `daemon`：`backend_takeover` Hook 在 reactor 0 上对每个导出 `run_on_reactor(takeover())`
  （沿用 `start()` 的调用方式，`src/server/daemon.cpp:100`）。
- 外部脚本：`[cluster] takeover_hook` 非空时，在后端钩子之后 `fork/exec` 该脚本
  （offload 线程，超时 `fence_lease`，超时/非零退出只 WARN），环境变量
  `LNFS_CLUSTER_ID / LNFS_NODE / LNFS_EPOCH / LNFS_PREV_NODE`（来自围栏旧记录）。
  Lustre 的 `lctl set_param mdc.*.evict_client=<nid>` 就放在这里（09 §9.7 Lustre 条）。

**测试**：`tests/test_backend.cpp` 默认 `takeover()` 返回成功；`test_cluster_controller`
用可执行的临时脚本验证环境变量与超时。

### D2 CephFS reclaim 原语

**目标**：09 §9.7 CephFS 条：以同一 uuid 接管旧会话，MDS 立即驱逐并释放 caps/锁。

**关键约束**：libcephfs 要求 `ceph_set_uuid` / `ceph_start_reclaim` / `ceph_finish_reclaim`
在 `ceph_init` 之后、`ceph_mount` **之前**调用；且 `ceph_start_reclaim(uuid)` 会驱逐当前持有
该 uuid 的会话。因此 **Standby 绝不能带 uuid 挂载**（否则会踢掉活动网关），原语只能在
接管时执行。

**改动点**

- `src/backend/cephfs/cephapi.hpp` 函数表加

  ```cpp
  int (*ceph_set_uuid)(ceph_mount_info*, const char* uuid) = nullptr;
  int (*ceph_start_reclaim)(ceph_mount_info*, const char* uuid, unsigned flags) = nullptr;
  void (*ceph_finish_reclaim)(ceph_mount_info*) = nullptr;
  inline constexpr unsigned kCephReclaimReset = 1;  // CEPH_RECLAIM_RESET
  ```

  三个符号在 `cephapi.cpp` 的 dlsym 表里标为**可选**（旧 libcephfs 缺失时钩子退化为
  WARN + 空操作）；`scripts/check_cephapi_abi.sh` 加这三个签名。
- `[export.cephfs] uuid`：`make_cephfs`（`src/backend/cephfs/cephfs.cpp:1664`）新增键；
  为空且集群模式 → `daemon` 在构造后端前自动填 `cluster.id + "-" + fsid`（各网关相同）。
- `CephBackend::start()` 不变（普通会话，无 uuid）。`CephBackend::takeover()`：
  `ceph_unmount` → `ceph_set_uuid(uuid)` → `ceph_start_reclaim(uuid, RESET)` →
  `ceph_finish_reclaim` → `ceph_mount(root)`；重挂期间的 inode/Fh 缓存整体失效
  （Standby 没有栈引用它们，安全）。`start_reclaim` 返回 `-ENOENT`（没有旧会话）视为成功；
  `-ENOTRECOVERABLE`/`-EOPNOTSUPP` 记 WARN 返回错误（控制器只告警）。
- Draining 后 `backend_reset`（C2）会 `stop()+start()`，会话回到无 uuid 状态。

**测试**

- `tests/cephapi_fake.{hpp,cpp}`：会话表加 `uuid`；`set_uuid/start_reclaim/finish_reclaim`
  语义——`start_reclaim(u)` 驱逐持有 `u` 的其他会话并释放其锁；注入接口
  `FakeCephApi::plant_stale_lock(path, owner_uuid, range)` 模拟故障网关残留锁；
  `reclaim_calls()` 计数。
- `tests/test_cephfs.cpp`：`takeover()` 前对残留区间 `lock()` 得 EAGAIN，`takeover()` 后成功；
  缺失符号时 `takeover()` 返回 ENOTSUP 而非崩溃。

---

## 阶段 E：验证与文档

### E1 `v4failover` 验收模式 + 本机双实例脚本

**改动点**

- `tests/accept_client.cpp` 新增 `cmd_v4failover(host, port_a, port_b, export, backing, takeover_cmd)`
  （仿 `cmd_v4reclaim`，`tests/accept_client.cpp:2172`）：
  1. 连 A：OPEN(CREATE) + LOCK 区间 + WRITE(UNSTABLE) 记录验证器；
  2. 执行 `takeover_cmd`（脚本负责 `kill -9 A` + `lightnfs-ctl -s B.sock cluster takeover`）；
  3. 连 B：SEQUENCE(旧 sessionid) → 断言 BADSESSION；EXCHANGE_ID 同 co_ownerid → 断言
     `server_owner`/`scope` 与 A 相同、clientid 高 32 位变化；CREATE_SESSION；
  4. 旧 stateid READ → STALE_STATEID；OPEN(CLAIM_PREVIOUS) → OK；LOCK(reclaim) → OK
     （若 B2 触发 DELAY 则循环重试，记录重试次数）；普通 OPEN → GRACE；
  5. COMMIT → 验证器不同 → 重发 WRITE(FILE_SYNC) → READ 逐字节比对 backing；
  6. RECLAIM_COMPLETE → 普通 OPEN 成功（提前出 grace）。
- 新脚本 `scripts/accept_failover_local.sh`：两个 `lightnfsd`（不同 `state_dir`、端口、ctl
  套接字，同一 `shared_dir` 与 backing 目录；`local` 后端 +
  `unsafe_skip_backend_checks = true`；backing 与 `shared_dir` 放在带 btime 的文件系统上，
  **不要用 tmpfs**，否则 fallback 句柄跨进程不稳定），A `role=active`、B `role=standby`
  `takeover=manual`；依次跑 `v4failover`、`wtest`（v3 在切换前后各一轮）、
  `cluster status` 断言 role/epoch，最后脑裂检查：手工把 `fence` 改写成 B 后，A 在
  `≤ 3 × fence_lease` 内自行 Draining（`lightnfs_cluster_fence_lost_total == 1`）。
  Release + ASAN 各跑一轮，沿用 `accept_m6_local.sh` 的骨架。

**验收**：脚本 0 退出；Activating 用时（C4 指标）< 1s；无 EIO/ESTALE。

### E2 fake 注入与脑裂演练

- `cephapi_fake` 的残留锁注入（D2）接到 `test_state` 的 B2 场景：残留锁在 `takeover()`
  后释放 → DELAY 次数 ≥ 1 且最终成功。
- `gfapi_fake` / `llapi_fake`：加"锁在 N 毫秒后自动释放"注入（模拟 ping-timeout /
  obd_timeout），验证 B2 在 grace 内等到释放。
- 脑裂：`test_cluster_controller` 已覆盖围栏被改写；E1 脚本覆盖真实进程。root VM 场景
  （keepalived + 内核客户端 + fsx/cthon）不进 CI，写成 `scripts/accept_failover_vm.sh`
  仿 `accept_m6_vm.sh`，三种后端各一轮，由人工触发。

### E3 文档

- `docs/design/08-config-observability.md`：`[cluster]` 段、`[export.cephfs] uuid`、
  按节点豁免键列表、新指标。
- `docs/deployment.md`：新增"多网关主备"一节——VIP + keepalived 示例（`notify_master`
  调 `lightnfs-ctl cluster takeover`、`notify_backup` 调 `cluster standby`）、`bind` 只绑
  VIP 的要求、NTP、`shared_dir` 权限、陈旧 `exports.<node>` 清理、Gluster
  `network.ping-timeout ≤ grace/2`、已知限制（DRC/槽缓存不复制、委托随故障消亡）。
- `docs/design/07-state-management.md` §7.5：稳定存储位置在集群模式下改为 `shared_dir`。
- `docs/design/05-backend-api.md`：`takeover()` 记入可选扩展。
- `docs/design/09-multi-gateway-failover.md`：状态从"方案，未实现"改为按阶段标注，
  §9.10 改动清单逐行链接到本册步骤。
- `docs/design/README.md` 目录加本册。

---

## 10.2 每步的合并门槛

| 门槛 | 要求 |
|------|------|
| 回归 | `ctest`（Release + ASAN）全过；`scripts/accept_m6_local.sh` 全过 |
| 新增测试 | 本步"测试"小节列出的用例全部存在且通过 |
| 单网关等价 | `[cluster]` 缺省时 `lightnfs-ctl status`、指标名集合、日志级别行为与合并前一致 |
| 格式/静态 | `scripts/format_check.sh`、`scripts/tidy.sh` 通过 |
| 文档 | 触及配置键或指标的步骤同步更新 08 册（E3 只做汇总） |

## 10.3 已知取舍与待定项

- **接管时 epoch 的 32 位截断**：clientid 高 32 位与 stateid.other 前 4 字节只用 epoch 低
  32 位（`state_mgr.cpp:144`），全局 epoch 每次接管 +1，2^32 次内不会回绕，不处理。
- **围栏用墙钟**：跨节点比较必须依赖 NTP；`fence_lease` 默认 3s 对应的 500ms 偏差容忍在
  E3 文档中写明。若后续把 `ClusterStore` 换成"经后端 API 写入导出树"的实现
  （09 §9.4），接口不变。
- **Standby 的后端已 `start()`**：CephFS 在 Standby 是一个无 uuid 的普通会话，只占一个
  MDS 会话槽，不持 caps；Gluster/Lustre 同理。若运维希望 Standby 完全不连集群，
  可在后续加 `[cluster] standby_connect = false`（延迟到 Activating 再 `start()`，
  代价是接管时间加上后端连接时间）——本计划不做。
- **多活/计划内迁移（09 §9.9）**：不在本册范围；C2 的控制器按"每进程一个角色"实现，
  演进到"每导出一个角色"时把 `ClusterStore` 的键空间加 `fsid` 前缀即可，接口预留但不实现。
