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
| | B2 reclaim 下推失败 → DELAY ✅ 2026-09-05 | 状态层 grace 内重试 | — |
| | B3 CLAIM_DELEG_PREV_FH ✅ 2026-09-05 | 接受为普通 open 状态 | — |
| | B4 导出表一致性摘要 ✅ 2026-09-05 | 启动期拒绝树不一致的网关入集群 | A1 A2 |
| C 进程生命周期 | C1 `ProtocolStack` 可重建 ✅ 2026-09-05 | 栈的构造/析构与进程解耦；连接收敛等待 | A4 |
| | C2 `ClusterController` 状态机 ✅ 2026-09-05 | Standby/Activating/Active/Draining + 围栏协程 | A1–A4 B1 C1 |
| | C3 ctl `cluster *` 命令 ✅ 2026-09-05 | status / takeover / standby | C2 |
| | C4 指标 ✅ 2026-09-05 | role / epoch / fence age / takeovers | C2 |
| D 后端接管钩子 | D1 `Backend::takeover()` 接口 + 脚本钩子 ✅ 2026-09-05 | 默认空操作；可配外部脚本（Lustre evict 等） | C2 |
| | D2 CephFS reclaim 原语 ✅ 2026-09-05 | cephapi 三入口、fake、`[export.cephfs] uuid` | D1 |
| E 验证与文档 | E1 `v4failover` 验收模式 + 本机双实例脚本 ✅ 2026-09-05 | 无 root 的端到端证明 | C3 D1 |
| | E2 fake 注入与脑裂演练 ✅ 2026-09-05 | 残留锁、围栏分离 | B2 C2 D2 |
| | E3 文档 ✅ 2026-09-05 | 08 册、deployment.md、07 §7.5、09 册状态更新 | 全部 |

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

### B2 reclaim 模式下的下推失败 → DELAY（已完成，2026-09-05）

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

**测试**：`StateMgr.ReclaimLockPushDelayInGrace`：fake `LockMgr` 前 N 次 `lock()` 回 EAGAIN——
名单内客户端 grace 内 LOCK(reclaim) 得 DELAY（无 stateid、无网关授予、`denied` 清空），重试至
N 次后成功；出 grace 后普通 LOCK 被拒 → DENIED（带存储侧冲突），迟到的 LOCK(reclaim) 走既有
NO_GRACE 门禁而非 DELAY（实现注：出 grace 后 reclaim 请求先被门禁拦下，"DENIED"只对非 reclaim 锁成立）。

### B3 CLAIM_DELEG_PREV_FH 接受为普通 open 状态（已完成，2026-09-05）

**目标**：09 §9.7 第 3 条，让 Linux 客户端找回委托的路径零退避。

**改动点**：`src/nfsv4/engine.cpp:1531` 的 `case kClaimDelegPrevFh`：从 NOTSUPP 分支移出，
按 RFC 8881 §18.16 该 claim 无附加参数；设 `is_reclaim = true`、`deleg_claim = false`，
走与 `kClaimPrevious` 相同的 `state_.open(args.reclaim=true)` 门禁（名单 + grace，否则
RECLAIM_BAD / NO_GRACE，`state_mgr.cpp:776`）；应答 `OPEN_DELEGATE_NONE`，不调用
`maybe_grant_read_deleg`。`kClaimDelegatePrev`（4.0 式带文件名）保持 NOTSUPP。
grace 期不授新委托已由 `maybe_grant_read_deleg` 的 `in_grace()` 判定覆盖
（`state_mgr.cpp:931`），无需改动。

**测试**：`Nfs4.ClaimDelegPrevFhReclaimsAsPlainOpen`——重启后名单内客户端
CLAIM_DELEG_PREV_FH → OK、`delegation_type == NONE`（即使带 WANT_READ_DELEG）、读写可用、
同 owner 二次 claim 合并到同一 open 状态；grace 内普通 OPEN 仍 GRACE；CLAIM_DELEGATE_PREV
仍 NOTSUPP；RECLAIM_COMPLETE 后 → NO_GRACE；新 grace 下未列名单客户端 → RECLAIM_BAD。

### B4 导出表一致性摘要（已完成，2026-09-05）

**目标**：09 §9.3 末条：fsid 相同而树不同的网关不得入集群。

**改动点**

- `core::canonical_exports_digest(const Config&)`（`src/core/config.hpp`；实现时改为从解析后的
  `Config` 取值而非 `ExportTable`，因为后端名与子表键值只在 `ExportConfig` 里，且摘要要在
  `ExportTable::build` 消费掉配置之前算出）：对每个导出（按 fsid 排序）取
  `path | fsid | backend | readonly | squash | anon_uid | anon_gid` 与后端子表中
  **除按节点豁免键之外**的所有 `key=value`（排序后拼接），整体做 SHA-256
  （`src/util/sha256.{hpp,cpp}`，FIPS 180-4 向量 + 填充边界测试）。豁免键列表是常量
  `kPerNodeBackendKeys = {conf, keyring, id, user, name, log_file, fd_cache, mon_host}`，
  放在 `core/config.hpp`（core 不能依赖 server/）并在 08 册记录；`canonical_exports_text`
  暴露摘要原文供诊断。
- `daemon` 启动（集群模式，backend `start()` 之前）：先 `list_exports_digests()` 逐一比对，
  任一不同 → 打印对方节点名与双方摘要，退出码 1，**不写入**自己的记录（否则一台配错的网关
  会让健康网关重启时也被拒）；一致后才 `put_exports_digest(node, digest)`。
  被移除节点的陈旧 `exports.<node>` 需要运维手动删除（deployment.md 说明；不做自动 GC）。

**测试**：`ClusterStore.Sha256Vectors`、`ClusterStore.ExportDigestIgnoresPerNodeKeys`
（两份只差凭证/日志/缓存/clients/QoS 且导出顺序不同的配置摘要相同；subdir/fsid/path/squash/
fs_name/volume/readonly 任一不同则不同）。端到端：两个网关共用 `shared_dir`，第二个改了
`subdir`（或 fsid）→ 启动被拒并打印对方节点名与双方摘要。

---

## 阶段 C：进程生命周期

### C1 `ProtocolStack` 可重建 + 连接收敛等待（已完成，2026-09-05）

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

**实现记录**：
- `activate/deactivate` 与 `DataPlaneInstance` 放在新文件 `src/server/data_plane.{hpp,cpp}`
  （而非 `daemon.cpp` 的匿名命名空间），`tests/test_daemon_lifecycle.cpp` 才能直接驱动。
- `Frontend::drain_connections(grace, close_timeout)`：`ConnRegistry::wait_idle(grace)` →
  `close_all()` → `wait_idle(close_timeout)` → 再等两个监听器的 `ConnTracker` 归零（连接协程在
  离开注册表之后还会碰一次 tracker）。单网关退出用 `kShutdownDrainGrace = 2s`；C2 用 `2 × fence_lease`。
- `Listener::wait_stopped()`、`CtlServer/MetricsHttp::start()+wait_stopped()`：accept 协程退出时
  signal，析构前必须 join——测试暴露了一个既有隐患：`Management` 若先于 runtime 销毁，ctl 的
  accept 循环会在已关闭的 fd 上死循环（`run_server` 的关停顺序恰好掩盖了它）。
- ctl 命令对数据面的引用改为**计数钉住**（`DataPlaneSlot::acquire()` → `PlaneRef`），
  `Management::detach()` 等待钉住数归零（上限 5s，超时告警），封掉 A4 留下的
  "state dump 中途 detach" 窗口。
- 租约扫描器改为 100ms 切片睡眠，`stop_lease_scanner()` 用 promise/future join，停栈延迟 ≤ ~100ms。
- 指标提供者：`obs::register_text_provider` 返回句柄、新增 `unregister_text_provider/
  text_provider_count`；`register_metrics_providers` 返回 RAII `MetricsRegistration`；
  `Frontend` 的缓冲池指标同样按句柄注销。

### C2 `ClusterController` 状态机与围栏协程（已完成，2026-09-05）

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

**实现记录（与上文的差异）**：
- 围栏 IO 是共享文件系统上的阻塞调用（A2 的约定：不得在 reactor 上跑），所以控制器**不用
  reactor 协程**，而是自带一个定时线程：每 `fence_lease` 调一次 `tick()`（Standby 轮询 /
  Activating 与 Active 续租）。`tick()` 公开，测试用内存 store 同步驱动状态机。
- 数据面工作（`activate/deactivate`、后端钩子）经 `Hooks::post` 投递到主线程事件循环
  （`daemon.cpp` 的 `MainLoop`：条件变量 + 队列，统一处理信号、SIGHUP reload 与投递的工作；
  测试里 `post` 为空 = 内联执行）。**Activating 期间控制器继续续租**，接管耗时再长也不会让
  围栏过期被别人抢走；投递的工作完成后才转 Active（失败则释放围栏、回 Standby、计数）。
- 自动接管策略：`takeover=auto && role!=standby` 时围栏缺失/过期/属于本节点旧化身即接管；
  `takeover=manual` 只响应 `request_takeover`；`role=standby` 永不自动接管。
- 集群模式进程启动不再 bump epoch（A3 的过渡逻辑已删除）：`cluster_identity` 只读 epoch 作标签，
  栈用接管时铸出的 epoch 构造（`bring_up(epoch)` 先写 `core.epoch`）。
- 退出路径：停控制器线程 → 跑完已投递的工作 → `take_down` → 若仍持围栏则 `release_fence` →
  后端 → 管理面 → runtime。
- ctl 的 `status role=` 已接到控制器（C3 再加 `cluster` 子命令）；快照里带 takeovers /
  fence_lost / activation_failures / last_activation，供 C4 出指标。

**测试**：`tests/test_cluster_controller.cpp`：用内存 `ClusterStore` fake + 记录调用序列的
Hooks——
- Standby 见围栏过期自动接管，Hooks 调用顺序 = fence → epoch → takeover → activate；
- Active 续租时围栏被改写 → deactivate → backend_reset → 不 release 他人围栏；
- `takeover=manual` 时围栏过期不动作，`request_takeover(force=true)` 覆盖未过期围栏；
- activate 失败 → 围栏释放、回 Standby、epoch 已 +1（允许，单调即可）。
- 另加：围栏连续 3 次 IO 失败 → Draining；`request_standby` 释放自己的围栏；投递（非内联）
  模式下 Activating 期间续租、Draining 等投递完成；定时线程在 1 个 lease 内完成接管。
- 端到端（本机双实例，同端口、同 `shared_dir`、`fence_lease=1s`）：A 先起持围栏，B 起后为
  standby 且不监听；`v4reclaim` 客户端在 A 上持有 open 状态 → `kill -9 A` → B 在 ~3s 内自动
  接管并在同一端口监听、epoch+1、从共享名单进 grace → 客户端 CLAIM_PREVIOUS 成功；B 退出时释放围栏。

### C3 ctl `cluster` 命令（已完成，2026-09-05）

**改动点**

- `src/server/ctl.cpp`：命令 `cluster status | cluster takeover [--force] | cluster standby`，
  `CtlDeps` 加 `ClusterController* cluster`（空 → `"cluster: not enabled"`）。
  `status` 文本：`role=active node=gw1 epoch=17 fence_owner=gw1 fence_age_ms=812 shared_dir=... peers=gw1,gw2`，
  `--json` 同字段。用法字符串（`src/server/ctl.cpp:93`）同步更新。
- `tools/lightnfs_ctl.cpp`：新增 `cluster` 子命令组（仿 `drc [flush]` 的带参叶子，
  `tools/lightnfs_ctl.cpp:194`）。

**测试**：`Ctl.AnswerCommandSurface` 加三条命令的文本/JSON 断言（fake 控制器）。

**实现记录（与上文的差异）**：
- 控制器的围栏/名单调用都是共享文件系统上的阻塞 IO（A2 约定），而 ctl 命令跑在 reactor 上，所以
  `cluster *` 放在 `CtlServer::answer_async`，`peers()` / `request_takeover` / `request_standby`
  经 `rt::offload` 在 offload 池执行；同步 `answer()` 只回答无控制器时的 `cluster: not enabled`。
- `ClusterController` 新增 `config()` 与 `peers()`（`list_exports_digests` 的节点名，排序）——
  "peers" 即向共享目录发布过导出摘要的网关，无需新的名单文件。
- `status` 行在计划字段之外再带 `fence_epoch` / `fence_expires_in_ms`（围栏到期倒计时，负值 =
  已过期）、`takeover`（策略）、`takeovers` / `fence_lost` / `activation_failures` /
  `last_activation_ms`（C4 的同源计数）；无围栏记录时文本为 `none`/`-`，JSON 为 `null`。
  `takeover` 失败时区分"非 standby（带当前角色）"与"围栏被 X 持有（提示 `--force`）"。
- `daemon.cpp`：控制器改为在 `Management::start` 之前构造（定时线程仍在管理面起来之后才
  `start()`），`Management::start` 多一个 `ClusterController*` 参数传给 `CtlDeps::cluster`；
  `status` 的 `role=` 从控制器直接派生，原 `role` 回调不再需要。
- 测试：`ClusterController` 不是接口，"fake 控制器"= 真控制器 + 内存 `ClusterStore`（从
  `test_cluster_controller.cpp` 抽到 `tests/mem_cluster_store.hpp` 共用）+ 内联钩子；
  `Ctl.AnswerCommandSurface` 只断言未启用集群的三条回答，其余在新用例 `Ctl.ClusterCommands`
  （需要 runtime 的 offload 池）：status 文本/JSON、围栏被他人持有时拒绝与 `--force`、
  active 时拒绝 takeover、standby 的钩子顺序 `activate deactivate reset` 与围栏释放、
  过期围栏免 force、peers 读失败时 `peers=?`/`null`。
- `lightnfs-ctl cluster <status|takeover|standby>` 为单个带位置参数的叶子（同 `drc [flush]`），
  `--force`/`-f` 是该叶子的布尔选项，原样转发到线协议。本机双实例（同端口、同 `shared_dir`、
  `takeover=manual`）手工验证：takeover → 对端拒绝并报持有者 → `--force` 抢占、原 active 自动
  draining → standby 释放围栏。

### C4 指标（已完成，2026-09-05）

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

**实现记录（与上文的差异）**：
- 提供者在 `ClusterController` 构造时注册、析构时注销（scrape 在注册表锁内跑提供者，注销返回后
  不会再有 scrape 进入），不经 `register_metrics_providers`——后者随协议栈重建。
- `lightnfs_cluster_activation_seconds` 用 `obs::LatencyHistogram`（100µs–5s 固定桶，与协议层
  直方图同桶），每次 Standby→Active 观测一次；不做 quantile。
- 额外系列：`lightnfs_cluster_fence_owned`（最近看到的围栏记录是否属于本节点——"active 却不持围栏"
  的告警条件）与 `lightnfs_cluster_activation_failures_total`。`fence_age_seconds` 在未见过任何
  围栏记录时不出样本（而非 0 或负值）。
- 测试：`Metrics.ClusterSeriesRenderWithControllerLifetime`（standby 初值 → 接管后 role/epoch/
  owned/age/直方图 → 围栏被夺后 fence_lost，控制器析构后提供者数回落、系列消失）；
  `ClusterController.StandbyTakesOverAFreeOrExpiredFence` 接管后 `takeovers_total == 1`。

---

## 阶段 D：后端接管钩子

### D1 `Backend::takeover()` 接口 + 外部脚本钩子（已完成，2026-09-05）

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

**实现记录（与上文的差异）**：
- `Hooks::backend_takeover` 改为接收 `TakeoverContext{ backend::ClusterIdentity identity;
  std::string prev_node; }`（`cluster_controller.hpp`）：`prev_node` 取自 Standby 最后一次 tick
  读到的围栏记录（至多一个 lease 旧），记录属于本节点旧化身或围栏为空时为空串——它是钩子要驱逐的
  死网关。
- 脚本执行在新文件 `src/server/takeover_hook.{hpp,cpp}` 的 `run_takeover_hook(path, identity,
  prev_node, timeout)`：用 `posix_spawn`（进程多线程，不用 `fork`），envp 为本进程环境去掉旧的
  `LNFS_*` 再加四个变量；`waitpid(WNOHANG)` 轮询，到时 SIGKILL 并回收。返回：退出 0 → ok；
  超时 → ETIMEDOUT；非零退出/信号 → EIO；起不来 → spawn 的 errno。每种失败都带脚本路径写 warn。
- 钩子由 `daemon.cpp` 组装：先在 reactor 0 上对每个导出 `run_on_reactor(backend->takeover(identity))`
  （同 `start()`），再跑脚本。它随控制器投递的接管工作在**主循环线程**执行（不是 reactor，阻塞无妨；
  接管期间控制器线程照常续租），没有另起 offload 线程。
- `kBackendApiVersion` 仍为 1；05 册 §5.10 记下"生命周期钩子默认成功、不设 Cap 位"的例外。
- 测试：`Backend.DefaultTakeoverIsANoOpThatSucceeds`（memory 后端）；
  `ClusterController.StandbyTakesOverAFreeOrExpiredFence` 断言上下文（空围栏 → `prev_node=""`，
  替换过期记录 → `prev_node=gw1`）；新增 `ClusterController.TakeoverHookScriptEnvAndTimeout`：
  临时脚本把四个变量写回文件、进程环境里的旧 `LNFS_PREV_NODE` 不透传、`sleep 30` 在 200ms 被杀
  返回 ETIMEDOUT、`exit 3` → EIO、缺失路径 → ENOENT、钩子失败控制器仍转 Active。

### D2 CephFS reclaim 原语（已完成，2026-09-05）

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

**实现记录（与上文的差异）**：
- 真实签名（Ceph 20.2 `libcephfs.h`，`check_cephapi_abi.sh` 已比对通过）：`ceph_set_uuid` 返回
  **void**，不是 int；`ceph_finish_reclaim` 也是 void。`kReclaimReset` 由脚本与 `CEPH_RECLAIM_RESET`
  静态断言。
- **顺序改为 unmount → `start_reclaim(uuid, RESET)` → `finish_reclaim` → `set_uuid(uuid)` → mount**：
  libcephfs 的 `Client::start_reclaim` 对"句柄自身已带的 uuid"直接回 EINVAL，所以 uuid 必须在回收
  **之后**才设到句柄上（Ganesha FSAL_CEPH 同序）。回收失败（EOPNOTSUPP/ENOTRECOVERABLE）时**不**
  `set_uuid`，让下次接管还能重试回收；同一会话再次 `takeover()`（uuid 已是自己的）只重挂不回收。
- uuid 为空时由后端在 `takeover()` 里按 `ClusterIdentity.cluster_id + "-" + fsid` 派生，不需要 daemon
  在构造前注入——各网关得到相同值，效果一致，少一处耦合。
- 重挂失败（`ceph_mount`/取根失败）：释放句柄与 UserPerm，导出回到"未启动"（`started()==false`、
  操作回 ENOTCONN），返回该错误；控制器 Draining 后的 `backend_reset` 用 `stop()+start()` 重建。
  `start()` 的挂载 + 取根尾段抽成 `mount_root()` 与 `takeover()` 共用。
- fake：会话注册表（`ceph_create`/`ceph_release`）、`ceph_mount_info.uuid/evicted`、
  `plant_stale_lock(path, uuid, start, len)` 造一个持 uuid 的"幽灵会话"及其排他锁，`start_reclaim`
  按真实语义（未 init → ENOTCONN、已挂载 → EISCONN、自身 uuid → EINVAL、无人持有 → ENOENT、
  `fail_reclaim(err)` 注入）驱逐持同 uuid 的其他会话并删其锁，被驱逐会话此后 `setlk` 回 ESHUTDOWN；
  `api_without_reclaim()` 给出缺三个符号的表（`complete()` 对可选项放行）；另有 `stale_locks()`/
  `reclaim_calls()`/`last_uuid()`。
- 测试：`Cephfs.TakeoverReclaimsStaleLocks`（残留锁 EAGAIN → takeover → 成功；他人 uuid 的锁不受影响；
  同会话二次 takeover 不再回收；stop/start 后会话无 uuid、再接管 ENOENT 视为成功；无泄漏）、
  `Cephfs.TakeoverExplicitUuidAndFailures`（显式 uuid；ENOTRECOVERABLE/EOPNOTSUPP 报错但仍服务且
  下次重试成功；重挂失败停用后 `start()` 恢复）、`Cephfs.TakeoverWithoutReclaimSymbolsOrMount`
  （ENOTSUP 且继续服务；未启动 → ENOTCONN）；`ConfigFactory` 加 `uuid` 键。本机无 Ceph 集群，
  真实库只核对了 `libcephfs.so.2`（20.2.0）导出这三个符号；集群验收留给 E1/`accept_cephfs.sh`。

---

## 阶段 E：验证与文档

### E1 `v4failover` 验收模式 + 本机双实例脚本（已完成，2026-09-05）

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

**实现记录（与上文的差异）**：
- `cmd_v4failover(host, port_a, port_b, export, backing, takeover_cmd)`：A 连 `port_a` 建
  OPEN(CREATE)+LOCK[0,100)+WRITE(UNSTABLE) 并记验证器，丢连接（不 CLOSE）；跑 `takeover_cmd`；
  连 `port_b`。断言链：先发一条只带旧 sessionid 的 SEQUENCE → **BADSESSION**（10052）；
  EXCHANGE_ID 同 co_ownerid → `server_owner`/`scope` 与 A 相同、clientid 高 32 位（epoch）变化；
  旧 stateid READ → **STALE_STATEID**；plain OPEN → **GRACE**；OPEN(CLAIM_PREVIOUS) → OK；
  LOCK(reclaim) 循环重试 **DELAY**（B2 下推兜底，本机 local 后端 0 次重试）；COMMIT 验证器变化 →
  WRITE(FILE_SYNC) 重发 → READ 与 backing 逐字节比对；RECLAIM_COMPLETE → plain OPEN OK。
  参数复用 `main` 的 `nfs_port`/`mount_port` 两个位置当 A/B 端口。
- `scripts/accept_failover_local.sh`：骨架仿 `accept_m6_local.sh`，但 workdir 落在
  `build/`（**不是 /tmp**——tmpfs 无 STATX_BTIME，fallback 句柄跨进程不稳定，客户端已用"按名重解析"
  兜底并打印一行 note）。每配置（Release、ASAN）跑：起 A(active/auto)+B(standby/manual)、断言
  B 不监听；v3 `wtest` 打 A；`v4failover`——takeover 脚本 `kill -9 A` 后**不带 --force**轮询
  `cluster takeover`（A 的围栏 3×lease 过期后才被接受，贴近真实运维）；断言 B `role=active
  epoch+1 takeovers=1 last_activation_ms<1000` 且 `lightnfs_cluster_takeovers_total==1`；
  v3 `wtest` 打 B；脑裂：A 以 standby 重起，手工把 `fence` 改写成 A → B 下次续租丢围栏、
  `≤3×fence_lease` 内 Draining（`fence_lost=1`、`lightnfs_cluster_fence_lost_total==1`、端口关闭），
  A 见记录属于自己→自动接管（epoch+2）；最后干净关停、A 退出释放围栏、无 `level=error` 行。
- `LNFS_BUILD_DIRS="dir:label …"` 可跳过内建的 rel/asan 构建、复用已有构建目录迭代；
  `now_ms` 用 `date +%s%N`/1e6（有的 `date` 忽略 `%3N`）。
- 未加 `accept_failover_vm.sh`（E1 只要求无 root 的本机证明；带 keepalived/内核客户端的
  VM 版在 E 阶段其余步骤再说）。

### E2 fake 注入与脑裂演练（已完成，2026-09-05）

- `cephapi_fake` 的残留锁注入（D2）接到 B2 场景：残留锁在 `takeover()` 后释放 → DELAY 次数 ≥ 1
  且最终成功。
- `gfapi_fake` / `llapi_fake`：加"锁在 N 毫秒后自动释放"注入（模拟 ping-timeout / obd_timeout），
  验证 B2 在 grace 内等到释放。
- 脑裂：`test_cluster_controller` 已覆盖围栏被改写；E1 脚本覆盖真实进程。root VM 场景
  （keepalived + 内核客户端 + fsx/cthon）不进 CI，写成 `scripts/accept_failover_vm.sh`
  仿 `accept_m6_vm.sh`，三种后端各一轮，由人工触发。

**实现记录（与上文的差异）**：
- B2 的状态层 DELAY 机制本身已由 `StateMgr.ReclaimLockPushDelayInGrace`（fake `LockMgr`）覆盖；
  E2 用**各后端真实的锁实现 + 残留锁注入**把同一条路径端到端跑一遍。三个后端共享
  `tests/reclaim_probe.hpp` 的 `ReclaimProbe`：起一个已进 grace、名单里有"死网关客户端"的 `StateMgr`
  （gateway A 只 `confirm_create_session` 落名单，gateway B `load_grace_list`），
  `native_locks.manager/resolve` 指向被测后端，驱动 OPEN(CLAIM_PREVIOUS) + 反复 LOCK(reclaim)，
  返回每次状态与 `native_lock_reclaim_delays` 计数。
- `Cephfs.ReclaimLockDelayUntilTakeover`：`plant_stale_lock` 造死网关的排他锁 → LOCK(reclaim) 得
  DELAY（≥1 次，未铸 state）→ `takeover()` 回收死会话、MDS 放锁 → 下一次 reclaim 成功，全程在 grace。
- `Gluster.ReclaimLockDelayUntilBrickTimeout`：`gfapi_fake` 加 `plant_stale_lock` /
  `release_stale_locks_after(ms)`（定时线程删幽灵段，模拟 brick ping-timeout；`join_stale_timer`
  收线程）→ DELAY → 定时释放后 `lock_until_settled` 在 grace 内重试成功。Gluster 无 `takeover()`
  回收原语（锁随 TCP 连接释放），DELAY 重试即全部。
- `Lustre.ReclaimLockDelayUntilClientEviction`：Lustre 原生锁是真实 OFD `fcntl`，故不改 fake——
  用另一个描述符对 backing 文件加真实 `F_OFD_SETLK` 写锁当残留，`~150ms` 后关 fd 释放（模拟
  obd_timeout 驱逐），验证 DELAY → 成功。
- root VM 脚本 `scripts/accept_failover_vm.sh`（人工、非 CI）：真实内核 `mount -o vers=4.1`，两网关
  共享 `shared_dir` 与导出树，`flock` 持锁 + 写 → `kill -9 A` → `cluster takeover` B → 校验数据存活、
  可重新加锁、可续写；`local` 轮无需集群（任意 root VM 可跑），`gluster/cephfs/lustre` 轮由
  `LNFS_*` 环境变量提供集群配置，各一轮。本机无 root/内核挂载，仅 `bash -n` 语法校验。

### E3 文档（已完成，2026-09-05）

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


**实现记录**：
- deployment.md 新增 `## 5. 多网关主备（高可用）`（keepalived `notify_master/backup/fault`
  挂 `cluster takeover/standby`、拓扑图、`bind` 只绑 VIP、NTP、`shared_dir` 权限、陈旧
  `exports.<node>` 手工清理、Gluster `network.ping-timeout ≤ grace/2`、已知限制），
  原"已知限制"顺延为 `## 6.`。
- 07 §7.5 加集群模式稳定存储改到 `shared_dir` 的段落；08 加 `[export.cephfs] uuid` 键注释
  （`[cluster]`/豁免键/指标此前各步已就位）；05 §5.10 的 `takeover()` 例外此前 D1 已记。
- 09 册状态由"方案，未实现"改为"已实现（按阶段）"，§9.10 改动清单加"步骤"列、每行以
  `[A1]…[E3]` 引用式链接指向本册；§9.11 标注各验证项的落地文件；design/README 目录把 09
  标为"已实现"、10 标为"A–E 全部完成"。
- 纯文档改动，不涉代码与测试。
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
