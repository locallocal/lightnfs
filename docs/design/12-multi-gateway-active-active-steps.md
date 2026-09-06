# 12. 多网关多活（每导出一个活动网关）——实现步骤拆分

> 状态：**实施计划，未开始**。本册把 [11 册](11-multi-gateway-active-active.md) 的方案拆成可独立
> 合并、可独立验证的步骤；每步给出改动点（带现有代码锚点）、接口形态、测试与验收标准。
> 11 册回答"做什么、为什么"，本册只回答"按什么顺序、改哪里、怎么证明做对了"。体例沿用
> 09 册的实施计划（原 10 册，已完成后撤下，见 git 历史）。
>
> 前置：09 册全部已实现（§9.10 改动清单）：`ClusterStore`（`server/cluster_store.*`）、
> `ClusterController`（`server/cluster_controller.*`）、可重建数据面（`server/data_plane.*`）、
> `Backend::takeover()`、CephFS per-fsid 会话 uuid、`lnfs_accept_client v4failover` 与
> `scripts/accept_failover_local.sh`。

## 12.0 总原则

1. **默认零行为变化**：新增 `[cluster] mode`，默认 `failover`（09 的行为逐字节不变）；本册全部
   改动挂在 `mode = "active-active"` 之后。回归门：现有 ctest + `scripts/accept_m6_local.sh`
   （单网关）+ `scripts/accept_failover_local.sh`（主备）在每一步合并后都必须原样通过。
2. **一步一个 PR，可单独回滚**：步骤之间只允许向前依赖（§12.1 依赖图），不允许一个 PR 横跨
   两个阶段。
3. **先加维度、不改语义**：先给 `ClusterStore` / `StateMgr` / `ClusterController` 加 `fsid`
   维度（阶段 A、C），failover 模式下继续走既有的全局键与全局 grace——多活的
   "一个网关持所有 fsid 围栏"必须与单机等价（11 §11.13 的回归门），这条在 C1 合并后就要
   用 `accept_active_active_local.sh` 的单网关段证明。
4. **每步自带测试**：单元测试进 `tests/`，跨进程场景进 `lnfs_accept_client` + `scripts/`；
   没有测试的步骤不合并。
5. **CephFS 优先落地**：多活的端到端证明用本机 local 后端（`unsafe_skip_backend_checks`）+
   `cephapi_fake`；Gluster / Lustre 只做 11 §11.6 的配置校验，不做端到端。

### 本册对 11 册的实现细化（11 册未定或留给实现的点）

| 点 | 决定 | 依据 |
|----|------|------|
| 多活下网关自有 epoch 存哪 | 共享目录 `epoch.<node>`（与 `exports.<node>` 并列），**进程启动时** +1；协议栈在启动时建一次、不随 fsid 接管重建 | 11 §11.5 "epoch/身份仍按网关"；各网关是不同 server，epoch 无需全局 |
| 无属主 fsid 的应答 | 状态类 op 回 `NFS4ERR_DELAY`，而非 MOVED 指空 `fs_locations`（Linux 客户端对空 locations 直接 EIO） | 11 §11.12 "客户端按 grace 语义重试"的可行实现 |
| per-fsid reclaim 名单何时写 | 客户端在 F 内**首次铸出 stateid**（OPEN/LOCK）时 `put`；该客户端在 F 内状态清空或客户端过期时 `erase`；不再在 CREATE_SESSION 写 | 名单语义 = "在 F 持有状态、可能 reclaim 的客户端"（07 §7.5） |
| 网关存活判定 | `fence.<node>` 批量记录本身就是心跳：不持任何 fsid 的网关也按周期写空集记录 | 11 §11.3 批量续租；备选顺位判定需要"上一顺位是否活着" |
| 计划内迁移的网关间通道 | 只经 `ClusterStore`：源写 `fs/<F>/owner = T` 后释放围栏；T 的轮询看到 owner 指向自己且围栏空闲即接管。**不加网关间 RPC** | 11 §11.7 流程；沿用"lightnfs 不选主、不通信，只有围栏与接管动作" |
| 多活下 `[cluster] role` | 必须为 `auto`（显式 active/standby 为 EINVAL）；`takeover = manual` 仍有效，含义变为"只认 `ctl cluster takeover <fsid>`" | 每进程一个角色的键在 per-fsid 模型下无意义 |

## 12.1 阶段与依赖

| 阶段 | 步骤 | 交付物 | 依赖 | 11 册阶段 |
|------|------|--------|------|-----------|
| A 基础设施（无行为变化） | A1 多活配置键 ✅ 2026-09-06 | `mode` / `node_address` / `[[export]] nodes` 解析、校验、Gluster/Lustre 同卷约束 | — | P1 |
| | A2 `ClusterStore` per-fsid 键空间 ✅ 2026-09-06 | `fs/<fsid>/{epoch,owner,clients/}`、批量 `fence.<node>`、`nodes/<node>`、`epoch.<node>` | — | P1 |
| | A3 `StateMgr` per-fsid grace ✅ 2026-09-06 | `fsid → {deadline, reclaim_set}`；名单钩子加 fsid；`release_fsid()` | A2 | P1 |
| B 协议面 | B1 多活身份 + `eir_flags` | `server_owner.major_id` 按 node 派生；`SUPP_MOVED_REFER\|MIGR` | A1 | P2 |
| | B2 `fs_locations` / `fs_locations_info` 属性 | `attrs.cpp` 两属性编码；属主视图 `FsOwnerView` | A1 A2 | P2 |
| | B3 非属主导出边界回 `NFS4ERR_MOVED` | `engine.cpp` 的 fsid 门禁；referral 例外（LOOKUP、GETATTR fs_locations） | B2 | P2 |
| C per-fsid 控制器 | C1 `ClusterController` per-fsid 角色机 + 批量续租 | 每 fsid 一个 `{Remote, Activating, Active, Draining}`；一条续租协程 | A1–A3 B1 | P3 |
| | C2 per-fsid 自动接管 | 按 `nodes` 顺位接管过期围栏；per-fsid 后端 takeover；写 owner | C1 | P3 |
| | C3 `SEQ4_STATUS_LEASE_MOVED` | 属主变更后一个租约期对受影响客户端置位 | A3 C1 | P3 |
| | C4 ctl 与指标 | `cluster status` per-fsid 表；`cluster takeover/standby <fsid>`；`lightnfs_cluster_fs_*`、`lightnfs_v4_moved_total` | C1 C2 | P3 |
| D 计划内迁移 | D1 `ctl cluster migrate <fsid> <node>` | 源端 Draining → owner → 释放；目标端按 owner 接管 | C2 C3 C4 | P4 |
| | D2 滚动升级脚本 | `scripts/cluster_roll.sh`：逐 fsid 迁出 / 迁回 | D1 | P4 |
| E 验证与文档 | E1 `v4moved` 验收模式 + 三实例本机脚本 | 无 root 的端到端：referral、migration、单网关退化、猝死分散接管 | B3 C2 C3 D1 | P5 |
| | E2 fake 演练 | CephFS per-fsid uuid 回收只影响该 fsid；同卷约束 | A1 C2 | P5 |
| | E3 文档 | 08 册、deployment.md、07 §7.5、09 §9.9、11 册状态、design/README | 全部 | P5 |

关键路径：A2 → A3 → C1 → C2 → D1 → E1。阶段 B 三步与阶段 A 并行无冲突（B2/B3 在 C1 之前用
一个固定的属主视图做单元测试）。

---

## 阶段 A：基础设施

### A1 多活配置键（已完成，2026-09-06）

**目标**：解析并校验 11 §11.10 的三个新键；`mode = "failover"`（默认）时后两个键被忽略。

**改动点**

- `src/core/config.hpp`：`ClusterConfig`（`config.hpp:126`）新增

  ```cpp
  std::string mode = "failover";  // failover | active-active
  std::string node_address;       // 多活：本网关对外自有地址 "host:port"，写入 owner 记录
  ```

  `ExportConfig`（`config.hpp:107`）新增 `std::vector<std::string> nodes;`（属主优先级列表，
  空 = failover 模式）。`ExportEntry`（`config.hpp:167`）带上 `nodes`，供控制器与 ctl 使用。
- `src/core/config.cpp`：`[cluster]` 段分发（`config.cpp:326`）加两个键；`[[export]]` 段
  （`Section::kExport`）加 `nodes` 数组解析，复用 `clients` 数组的解析写法。
- `validate_config`（`config.cpp:438`）在 `cluster.enabled && mode == "active-active"` 时：
  - `node_address` 非空且形如 `host:port`（不解析 DNS）；
  - `role` 必须为 `auto`；`takeover` 取值不变；
  - 每个 `[[export]]` 的 `nodes` 非空、无重复；成员名字集合在**运行期**与 `nodes/<node>`
    记录核对（A2），配置期只查语法（`[A-Za-z0-9_.-]{1,64}`）；
  - **同卷同进退**（11 §11.6）：后端为 `gluster` 的导出按 `backend_config.values["volume"]`
    （`gluster.cpp:1696`）分组、`lustre` 按 `values["mount"]`（`lustre.cpp:415`）分组，同组
    内所有导出的 `nodes` 必须逐项相同，否则 EINVAL 并指出哪两个 fsid 冲突；
  - `mode = "failover"` 时若出现 `nodes` / `node_address` → WARN 忽略（不 EINVAL，方便逐台
    切换配置）。
- `server/daemon.cpp` 的 `restart_required_report`：`mode` / `node_address` / 任一导出的
  `nodes` 变化 → `"cluster settings changed: restart required"`。
- 多活模式的后端能力校验与 09 A1 相同（`kStableHandles` + `kByteLocks` + `native_locks`；
  `unsafe_skip_backend_checks` 放宽）。
- 实现注（2026-09-06）：`nodes` 非空时进入导出表摘要（`canonical_exports_text` 加一行
  `nodes=a,b,c`）——属主顺位是集群身份的一部分，各网关必须一致；failover 配置的摘要不变。
  `ExportTable::reload_dynamic` 对 `nodes` 变化报 restart required、不热应用。在 C1 落地前
  `run_server` 对 `mode = active-active` 直接拒绝启动（`--check-config` 照常返回 0），避免
  静默按主备运行。校验失败的原因以 WARN 日志给出（`validate_config` 只返回 EINVAL）。

**测试**：`tests/test_ctl.cpp` 仿 `Ctl.ClusterConfigKeys`（`test_ctl.cpp:225`）加
`Ctl.ActiveActiveConfigKeys`——默认 failover；`nodes` 缺失 / 重复 / 空被拒；`node_address` 缺
端口被拒；显式 `role = active` 被拒；Gluster 同卷不同 `nodes` 被拒、同 `nodes` 通过；
failover 模式下 `nodes` 只告警。

**验收**：`lightnfsd --check-config` 对 11 §11.10 示例返回 0；09 的示例配置与单网关配置结果不变。

### A2 `ClusterStore` per-fsid 键空间与批量续租（已完成，2026-09-06）

**目标**：11 §11.3 的键空间，**只加不改**——09 的全局 `epoch` / `fence` / `clients/` 方法原样保留
给 failover 模式。

**改动点**

- `src/server/cluster_store.hpp`（`cluster_store.hpp:37` 起）新增记录与方法：

  ```cpp
  struct OwnerRecord { std::string node; std::string address; uint64_t fs_epoch; };
  struct NodeFences {  // 一个 node 的批量围栏记录（也是它的心跳）
    std::string node; int64_t expires_at_ms; std::vector<uint32_t> fsids;
  };

  // per-node
  virtual Result<uint64_t> bump_node_epoch(std::string_view node) = 0;   // epoch.<node>
  virtual Result<void> put_node_address(std::string_view node, std::string_view addr) = 0;  // nodes/<node>
  virtual Result<std::vector<std::pair<std::string, std::string>>> list_nodes() = 0;
  // per-fsid 围栏（接口 per-fsid，实现批量）
  virtual Result<std::optional<FenceRecord>> read_fs_fence(uint32_t fsid) = 0;
  virtual Result<FenceRecord> acquire_fs_fence(uint32_t fsid, std::string_view node,
                                               std::chrono::milliseconds ttl, bool force) = 0;
  virtual Result<void> renew_fences(std::string_view node, std::chrono::milliseconds ttl) = 0;  // 一次写
  virtual Result<void> release_fs_fence(uint32_t fsid, std::string_view node) = 0;
  virtual Result<std::vector<NodeFences>> list_fences() = 0;  // 所有 fence.<node>，含过期的
  // per-fsid epoch / owner / reclaim 名单
  virtual Result<uint64_t> bump_fs_epoch(uint32_t fsid) = 0;
  virtual Result<std::optional<OwnerRecord>> read_owner(uint32_t fsid) = 0;
  virtual Result<void> put_owner(uint32_t fsid, const OwnerRecord&) = 0;
  virtual Result<std::vector<std::string>> list_clients(uint32_t fsid) = 0;
  virtual Result<void> put_client(uint32_t fsid, std::string_view owner_id) = 0;
  virtual Result<void> erase_client(uint32_t fsid, std::string_view owner_id) = 0;
  ```

- POSIX 实现（`cluster_store.cpp`）布局：

  ```
  shared_dir/
    hmac.key, exports.<node>          # 不变
    epoch.<node>                      # 该网关自有 epoch（多活）
    nodes/<node>                      # "<node_address>\n"，启动时写
    fence.<node>                      # "<expires_at_ms> <fsid>[,<fsid>...]\n"；空集 = 纯心跳
    fence.lock                        # 复用 09 的 O_EXCL 锁文件串行化所有 fence.<node> 改写
    fs/<fsid>/epoch                   # 接管代数
    fs/<fsid>/owner                   # "<node> <address> <fs_epoch>\n"
    fs/<fsid>/clients/<fnv64>         # 名单，命名同 09
  ```

  - `acquire_fs_fence(F)`：取 `fence.lock` → 读全部 `fence.<node>` → 若有**未过期**记录列出 F
    且 node ≠ 自己且 `!force` → EBUSY；否则把 F 加进自己的记录并写 `expires_at = now + ttl`。
  - `renew_fences`：只重写自己的 `fence.<node>`（一次 `atomic_write_file`，与 fsid 数无关）。
  - `read_fs_fence(F)`：扫描 `fence.<node>`，返回列出 F 的**未过期**记录（过期容忍
    `kFenceSkewTolerance`）；无 → nullopt。
  - `release_fs_fence(F)`：从自己的记录里去掉 F；不是自己的 → EPERM。
  - `fs/<fsid>/` 目录按需建；`epoch` 与 09 一样带 `.lock`。
- `tests/mem_cluster_store.hpp` 同步加这些方法（内存 map；`fence.<node>` 可被测试改写为
  "另一个 node 持有 F"或直接过期）。

**测试**：`tests/test_cluster_store.cpp` 加 `ClusterStore.FsFenceBatchedPerNode`——A 取 F1、F2，
记录只有一个文件；B 取 F1 得 EBUSY、取 F3 成功；A 过期后 B 取 F1 成功且 A `renew` 后
`read_fs_fence(F1)` 仍是 B；`release(F2)` 后 A 记录只剩空集但文件仍在（心跳）；owner/fs epoch/
名单往返；`list_nodes` 返回地址。

- 实现注（2026-09-06）：与上面接口草案的差异——`acquire_fs_fence` 多一个 `epoch` 参数（与 09 的
  `acquire_fence` 对齐，围栏记录随身带 fs epoch，`read_fs_fence` 不必再读 `fs/<fsid>/epoch`），
  记录格式因此为 `"<expires_at_ms> <fsid>:<epoch>[,...]"`；另加 `read_node_epoch` /
  `read_fs_epoch` 两个只读方法。`acquire` 成功时会把该 fsid 从**其他所有** node 的记录里剥掉
  （过期的也剥），否则旧属主回来续租心跳会让过期记录复活、出现双属主；`read_fs_fence` 在无
  活记录时返回最近过期的那条，供控制器看到"谁失联了"。`fence.<node>` 与 09 的 `fence` 共用
  `fence.lock`。名单/计数器的文件格式与 09 完全相同（`list_clients_in` 等按目录参数化）。
  `tests/mem_cluster_store.hpp` 同步实现，新增 `age_out_node` / `fs_taken_by` 两个测试旋钮。
  第二个测试 `ClusterStore.PerFsidEpochOwnerClientsAndNodes` 覆盖 node epoch、地址、fs epoch、
  owner 记录与 per-fsid 名单（含与全局名单/摘要/围栏文件互不干扰）。

**验收**：09 的 `ClusterStore.*` 测试与 `accept_failover_local.sh` 不变。

### A3 `StateMgr` per-fsid grace（已完成，2026-09-06）

**目标**：11 §11.5——grace / reclaim 名单按 fsid；epoch、clientid、stateid、写验证器逻辑不动。

**改动点**

- `src/state/state_mgr.hpp`：
  - `Config::StableStore`（`state_mgr.hpp:195`）三钩子加 `uint32_t fsid` 首参：
    `load(fsid)` / `put(fsid, owner)` / `erase(fsid, owner)`。failover / 单网关模式的既有实现
    忽略 fsid（`protocol_stack.cpp:21` 的 `cluster_stable_store` 与 `state_dir/clients/` 本机路径）。
  - grace 状态从全局三元组（`grace_pending_` / `grace_deadline_` / `grace_active_`，
    `state_mgr.hpp:516`）变成 `std::unordered_map<uint32_t, GraceWindow>`，**`fsid = 0`
    表示"全部导出"**，即今天的全局 grace（单网关启动、failover 接管）。
  - API 加 fsid 维度并保留无参重载：

    ```cpp
    void load_grace_list();                 // = 对 exports 里每个 fsid 各 arm 一个窗口（现状语义）
    void load_grace_list(uint32_t fsid);    // 只 arm F：读 stable.load(F)，deadline = now + grace
    bool in_grace() const;                  // 任一窗口活跃（ctl status 用）
    bool in_grace(uint32_t fsid) const;
    bool end_grace(uint32_t fsid = 0);
    void note_reclaimed(uint32_t fsid, std::string_view owner);
    // 多活：本网关失去 F 的属主权 → 丢弃 F 内全部 open/lock/deleg（不写名单，不回调客户端），
    // 并给持有过 F 状态的客户端打上 lease_moved_until = now + lease（C3 用）
    void release_fsid(uint32_t fsid);
    ```

- `src/state/state_mgr.cpp`：
  - `load_grace_list`（`:178`）、`in_grace`（`:203`）、`note_reclaimed`（`:226`）、
    `reclaim_complete`（`:684`）改按 fsid；`RECLAIM_COMPLETE` 没有 fsid 参数（RFC 8881
    §18.51 是整 clientid 的），按"该客户端在名单里的所有 fsid 一并 note"处理。
  - 门禁处全部带 fsid：`open`（`:795`、`:807`）、委托授予（`:949`）、无状态写（`:1277`）、
    `lock`（`:1518`、`:1526`、`:1663`）——这些调用点已经拿到 `fsid`（`check_io` 的入参，
    `engine.cpp:1046`）。
  - 名单写点从 CREATE_SESSION（现在 `persist_client` 处）改为"F 内首次铸 stateid"（open /
    lock 铸 `StateRec` 处）；`unpersist_client` 改为"该客户端在 F 内最后一个 StateRec 销毁"或
    客户端过期时对其持有过的每个 fsid `erase`。failover 模式下为了保持既有行为，
    `fsid = 0` 路径仍在 CREATE_SESSION 写全局名单（07 §7.5 不变）。
  - `release_fsid(F)`：遍历 `StateRec` 表按 fsid 过滤（`StateRec` 已含 fsid，
    `state_mgr.hpp:111`），走既有的强制过期路径但**不**下推 unlock（存储侧残留由新属主的
    takeover 钩子处理，11 §11.6）；不动 clientid / 会话。
  - `status()`（`:2105`）与 dump（`:2113`）输出每 fsid 的 grace 剩余。

**测试**：`tests/test_state.cpp` 加
- `StateMgr.PerFsidGraceIndependent`：arm F1 不 arm F2 → F2 新 OPEN 正常、F1 回 GRACE；F1 名单
  客户端 CLAIM_PREVIOUS 成功、RECLAIM_COMPLETE 后 F1 出 grace，F2 不受影响；
- `StateMgr.StableStoreHooksPerFsid`：内存 map 三钩子按 fsid 分桶；首次 OPEN 才 `put`；
- `StateMgr.ReleaseFsidDropsStateKeepsClient`：`release_fsid(F1)` 后 F1 的 stateid 回
  BAD_STATEID、F2 的仍有效、会话仍在、`lease_moved_until` 已置。

既有 `StateMgr.GraceListPersistsAndEarlyExit`、`GraceReclaimGate`、`ReclaimLockPushDelayInGrace`
必须不改断言地通过（`fsid = 0` 路径）。

- 实现注（2026-09-06）：与上面草案的差异——
  - 名单写点由新增的 `Config::per_fsid_reclaim`（默认 false）切换，而不是按 fsid 判断：关时
    CREATE_SESSION 写全局名单（07 §7.5 原样）；开时全局名单不写，改为"客户端在 F 内首次铸
    stateid（OPEN / LOCK / 委托）时 `put(F)`、最后一个状态销毁或客户端过期时 `erase(F)`"，
    `ClientRec::fs_states` 按 fsid 计数支撑这一点。C1 在多活模式打开该开关。
  - 无钩子时的本机 per-fsid 名单目录：`state_dir/fs/<fsid>/clients/`（全局仍是 `state_dir/clients/`）。
  - `release_fsid` 是协程（要拿分片锁），返回丢弃的状态数；经 `unlink_state(rec, from_client,
    handover=true)` 走既有的销毁链，但**不下推原生 unlock、不删名单记录**；同时把该 fsid 的
    grace 窗口结束、给持有过状态的客户端置 `ClientRec::lease_moved_until`（C3 在 SEQUENCE 上
    报告；`state` dump 已带 `lease_moved=` 字段）。
  - `note_reclaimed(fsid, owner)` 没有单独暴露：`RECLAIM_COMPLETE` 是整 clientid 的（引擎接受
    `rca_one_fs` 但不按 fs 处理），一次完成对所有窗口生效。
  - `Stats::fs_grace` 列出活跃的 per-fsid 窗口；`end_grace(0)` 结束全部窗口（`grace-end` 语义
    不变），`end_grace(F)` 只结束 F。
  - 新增测试 `PerFsidGraceIndependent`（本机目录、两窗口并存、grace-end 分级）、
    `StableStoreHooksPerFsid`、`ReleaseFsidDropsStateKeepsClient`。

---

## 阶段 B：协议面

### B1 多活身份与 `eir_flags`

**目标**：11 §11.2——各网关是同一 `server_scope` 下的**不同** server；宣告支持 referral / migration。

**改动点**

- `src/server/protocol_stack.cpp` `derive_server_identity`（`:88`）：`mode == "active-active"`
  时 `server_owner.major_id = "lightnfs-cluster:<id>:<node>"`，`server_scope =
  "lightnfs-cluster:<id>"`（failover 仍两者同为 `"lightnfs-cluster:<id>"`）。
- 协议栈固化的 epoch：多活模式在 `daemon.cpp` 启动时 `bump_node_epoch(node)`（A2）而非
  Activating 时 bump（`daemon.cpp:107` 附近的注释一并改）。
- `EXCHANGE_ID` 应答：`Engine::op_exchange_id`（`engine.cpp:3179`）在多活时 `eir_flags |=
  EXCHGID4_FLAG_SUPP_MOVED_REFER (0x1) | EXCHGID4_FLAG_SUPP_MOVED_MIGR (0x2)`；把是否多活作为
  `Engine` 构造参数（`bool referrals`），不查全局配置。

**测试**：`tests/test_nfs4.cpp` 加 `Nfs4.ActiveActiveIdentityAndFlags`——两个 `Engine`（node gw1、
gw2）`EXCHANGE_ID` 的 major_id 不同、scope 相同、flags 含 REFER|MIGR；failover 与单网关的
flags 不含这两位。

### B2 `fs_locations` / `fs_locations_info` 属性与属主视图

**目标**：GETATTR 能对导出根回答"这个 fs 在谁那儿"。

**改动点**

- 属主视图（新文件 `src/core/fs_owner_view.hpp`）：控制器写、引擎读的无锁快照

  ```cpp
  enum class FsRole { kActive, kDraining, kRemote, kUnowned };
  struct FsOwner { FsRole role; std::string node, address; uint64_t fs_epoch; };
  class FsOwnerView {  // shared_ptr<const map> 的原子交换；引擎每 op 读一次
    std::shared_ptr<const std::unordered_map<uint32_t, FsOwner>> snapshot() const;
    void publish(std::unordered_map<uint32_t, FsOwner>);
  };
  ```

  单网关 / failover：视图为空，引擎视所有 fsid 为 `kActive`（零行为变化）。挂在
  `CoreState`，与 `cluster` 指针并列。
- `src/nfsv4/attrs.hpp` / `attrs.cpp`：常量加 `kFsLocations = 24`、`kFsLocationsInfo = 67`；
  `supported_attrs()`（`attrs.cpp:10`）仅在 `referrals` 打开时含这两位（用一个进程级开关或
  给 `supported_attrs` 加参数，与 B1 的 `Engine` 参数同源）；编码：
  - `fs_locations`：`fs_root` = 该导出在伪 fs 里的路径（`PseudoFs::for_export(fsid)` 逐级
    `parent` 拼出 pathname4 组件），`locations[0] = { server = [owner.address], rootpath =
    fs_root }`；`kUnowned` 时 `locations` 为空（B3 决定此时用 DELAY，不会走到这里，但编码
    要能表达）。
  - `fs_locations_info`（RFC 8881 §11.10.1）：`fli_flags = 0`、`fli_valid_for = lease`，一个 `fs_locations_item` 一个
    server，`fls_currency = -1`（未知）、`fls_info` 为空；其余按最小合法值。
- `AttrSource`（`attrs.cpp:79` 处的 `src.fsid` 一带）加 `const FsOwner* owner` 指针；
  `Engine` 组装 GETATTR 时（`engine.cpp:851`）对导出侧对象从视图取 owner 填入；伪 fs 对象
  （`src.fsid = 0`）不带 owner（伪根不是任何 fs 的 referral）。

**测试**：`tests/test_nfs4.cpp` 加 `Nfs4.FsLocationsEncoding`——视图里 fsid 2 由 gw2 持有：
对 `/export/b` 根 fh `GETATTR(fs_locations)` 解出 `fs_root = ["export","b"]`、server =
`gw2 地址`；`supported_attrs` 含 24/67；单网关模式下请求 24 → 位被忽略（现有语义）。

### B3 非属主导出边界回 `NFS4ERR_MOVED`

**目标**：11 §11.4 / §11.9——客户端跨进非属主导出时被引导走；伪根照常。

**改动点**

- `src/nfsv4/nfs4_types.hpp` `Status` 枚举加 `kMoved = 10019`（尚无）。
- `Engine::resolve`（`engine.cpp:148`）：`decode_v4` 之后、`backend->resolve` 之前加门禁：
  `fsid != 0` 且视图里该 fsid 非 `kActive` → 根据 `role`：
  - `kRemote` / `kDraining` → `Err(kMoved)`；
  - `kUnowned` → `Err(kDelay)`（本册细化）。

  `Resolved` 结构不变；错误经现有 `Result` 路径回到 op。
- referral 例外（RFC 8881 §11.10 / Linux `nfs4_get_referral` 的实际请求形状
  `PUTFH(parent) LOOKUP(name) GETATTR(fs_locations,...)`）：
  - 伪根 `LOOKUP` 跨进非属主导出（`engine.cpp:757` 的 `child->exp` 分支）：**成功**并把 cfh 设为
    该导出的**伪 crossing 节点** fh（`pseudo_fh(*child)`，本来就如此），不去后端取根 oid；
  - 对 crossing 节点（fsid 0 的伪节点，但 `node->exp` 非属主）的 `GETATTR`：请求掩码 ⊆
    `{fs_locations, fs_locations_info, fsid, rdattr_error, mounted_on_fileid, supported_attrs,
    fh_expire_type, type}` → 正常应答（`fsid` 填该导出的 fsid，让客户端看到 fs 边界）；
    掩码含其他位 → `kMoved`；`GETFH` 放行；其余 op → `kMoved`。
  - 伪根 `READDIR`（`engine.cpp:1344`）：非属主导出的条目照常列出，属性按上一条规则，
    请求了不允许的属性时该条目 `rdattr_error = kMoved`（RFC 8881 §18.23）。
  - 属主导出的 crossing 节点行为不变。
- 指标：`Engine` 计数 `moved_total[fsid]`（C4 暴露）。

**测试**：`tests/test_nfs4.cpp` 加 `Nfs4.MovedAtExportBoundary`——视图：fsid 1 Active、2 Remote、
3 Unowned：`PUTROOTFH LOOKUP(export) LOOKUP(b) GETFH GETATTR(fs_locations,fsid)` 全部成功且
fsid = 2；同 fh `GETATTR(size)` → MOVED；`OPEN` 在 b → MOVED；用 b 内文件的 v4 fh（另一个
`Engine` 铸的）`PUTFH READ` → MOVED；fsid 3 的根 → DELAY；fsid 1 一切如常；
`READDIR /export` 三条目 rdattr_error 正确。仿 `Nfs4.PseudoFsCrossingAndAttrs`
（`test_nfs4.cpp:419`）的搭建方式。

---

## 阶段 C：per-fsid 控制器

### C1 `ClusterController` per-fsid 角色状态机 + 批量续租

**目标**：11 §11.3 / §11.14——"每 fsid 一个角色 + 一条批量续租协程"；failover 模式下现有机器
原封不动。

**改动点**

- `src/server/cluster_controller.hpp`：加一个并列的类，不改 09 的 `ClusterController`：

  ```cpp
  class FsClusterController {  // mode = active-active
   public:
    struct Hooks {
      std::function<void(std::function<void()>)> post;      // 同 09
      // 对单个 fsid：arm grace（StateMgr::load_grace_list(fsid)）、更新视图；失败 → 回 Remote
      std::function<Result<void>(uint32_t fsid, uint64_t fs_epoch)> activate_fs;
      // 对单个 fsid：视图置 Draining → StateMgr::release_fsid(fsid) → 视图置 Remote
      std::function<void(uint32_t fsid)> deactivate_fs;
      // 该导出的 Backend::takeover() + 外部脚本（LNFS_FSID 环境变量）
      std::function<Result<void>(uint32_t fsid, const TakeoverContext&)> backend_takeover;
    };
    struct FsState { FsRole role; uint64_t fs_epoch; std::optional<FenceRecord> fence;
                     std::optional<OwnerRecord> owner; uint64_t takeovers, fence_lost; };
    FsClusterController(const core::ClusterConfig&, const core::ExportTable&,
                        ClusterStore&, core::FsOwnerView&, Hooks);
    void start(); void stop(); void tick();   // 一次 tick：renew_fences + 每 fsid 一步
    Result<void> request_takeover(uint32_t fsid, bool force);   // C4
    Result<void> request_release(uint32_t fsid);                // C4（cluster standby <fsid>）
    Result<void> request_migrate(uint32_t fsid, std::string_view target);  // D1
    std::vector<FsState> snapshot() const;
  };
  ```

- 状态机（每 fsid）：

  ```
  Remote ──(围栏空闲/过期 且 轮到自己 | ctl takeover)──▶ Activating ──▶ Active
     ▲                                                                   │
     └──────── Draining ◀──(围栏丢失 | ctl standby/migrate | 退出)────────┘
  ```

  - `tick()`：先 `renew_fences(node, ttl)`（无论持有几个 fsid，每周期一次写；失败计数同 09 的
    `renew_failures_`）；再 `list_fences()` + 每个 fsid 读 owner，构造新视图 `publish`；
    Active 的 fsid 若 `read_fs_fence` 不是自己 → `Draining`（`fence_lost++`，不释放）。
  - 数据面：多活模式在 `daemon.cpp` 启动时 `activate()` 一次（`daemon.cpp:471` 的单网关分支），
    协议栈常驻；`FsClusterController` 只经三个 per-fsid 钩子操作 `StateMgr` 与视图，
    **不**调 `data_plane::activate/deactivate`。
  - 进程退出：对每个 Active 的 fsid 走 Draining（视图置 Draining → 释放围栏），最后
    `release_fs_fence` 逐个 + 保留 `fence.<node>` 空记录不删（下次启动覆盖）。
- `daemon.cpp`：`cluster_cfg.mode == "active-active"` 分支：启动时 `bump_node_epoch`、
  `put_node_address`、`put_exports_digest`，建栈，再建 `FsClusterController`（`daemon.cpp:486`
  的 controller 组装旁边加分支）。

**测试**：`tests/test_cluster_controller.cpp` 加 `FsClusterController.*`（用 `MemClusterStore`，
`tick()` 手动驱动，钩子记录调用序）：
- `SingleNodeOwnsEveryFsid`：三个导出 `nodes = ["gw1"]`，gw1 三次 tick 后三个 fsid 全 Active、
  一个 `fence.gw1` 记录列出三者、视图三条 Active——**这就是 11 §11.13 的单网关退化门**；
- `RenewIsOneWritePerTick`：持有 N 个 fsid 时每 tick 的 store 日志只有一次 `renew`；
- `ActiveLosesOneFsidDrainsOnlyThat`：把 `fence.gw2` 改成列出 F2 → gw1 对 F2 Draining →
  `deactivate_fs(2)` 被调、F1/F3 仍 Active。

**验收**：`accept_failover_local.sh` 不变（走的仍是 09 的 `ClusterController`）。

### C2 per-fsid 自动接管

**目标**：11 §11.8——按 `nodes` 顺位接管过期围栏，接管负载自然分散；接管钩子 scope 到单 fsid。

**改动点**

- `FsClusterController::tick()` 的 Remote 分支，对 fsid F（`takeover = auto` 时）：
  1. `read_fs_fence(F)` 未过期 → 只更新视图（owner 来自 `read_owner(F)`，若 owner 缺失或
     与围栏 node 不一致，以围栏为准、地址取 `nodes/<node>`）。
  2. 过期 / 无记录 → 计算顺位：F 的 `nodes` 里排在自己前面的每个 node，若其 `fence.<node>`
     **未过期**（活着）则**让位**（不接管）；例外：F 已无属主超过 `2 × 3 × fence_lease` 仍无人
     接（前位活着但不接，如 `takeover = manual`），则跳过前位接管。自己不在 F 的 `nodes` 里
     → 永不自动接管（ctl `--force` 可以）。
  3. 接管 = 09 `begin_activation` 的 per-fsid 版：`acquire_fs_fence(F)` → `bump_fs_epoch(F)` →
     `post(run_activation_fs)`：`backend_takeover(F, ctx)`（`prev_node` = 被替换记录的 node）→
     `activate_fs(F, fs_epoch)` → `put_owner(F, {self, node_address, fs_epoch})` → 视图 Active；
     任一步失败 → `release_fs_fence(F)`、回 Remote、`activation_failures++`。
     围栏在 post 期间由 tick 的批量续租自然续着（记录已含 F）。
- `daemon.cpp` 的 `hooks.backend_takeover` per-fsid 版（`daemon.cpp:497` 旁）：只对
  `exports.by_fsid(F)->backend` 调 `takeover()`（CephFS 的 `uuid = <cluster id>-<fsid>` 已保证
  只回收 F 的旧会话，09 D2），再跑 `[cluster] takeover_hook`，环境变量加 `LNFS_FSID`
  （`takeover_hook.hpp:7` 的列表加一行）。
- `takeover = manual`：Remote 分支只更新视图，不接管。

**测试**：`tests/test_cluster_controller.cpp` 加
- `FsClusterController.TakeoverFollowsNodeOrder`：F 的 `nodes = [gw1,gw2,gw3]`，gw1 记录过期、
  gw2 记录活着 → gw3 不接；gw2 记录也过期 → gw3 接管，钩子序 = takeover(F) → activate_fs(F)
  → put_owner，`prev_node = gw1`；
- `FsClusterController.DeadNodeSpreadsAcrossSuccessors`：gw1 持 F1/F2，`nodes` 分别为
  `[gw1,gw2,gw3]` / `[gw1,gw3,gw2]`，gw1 过期 → gw2 得 F1、gw3 得 F2（两个控制器共用一个
  `MemClusterStore` 交替 tick）；
- `FsClusterController.StuckUnownedFsidSkipsIdlePredecessor`：前位活着但 `2×ttl` 不接 → 后位接。
- `tests/test_cephfs.cpp` 复用 `reclaim_probe.hpp`：同一进程两个 CephFS 导出（uuid 不同），
  对 F2 `takeover()` 后 fake 只记录 F2 的 uuid 被 `ceph_start_reclaim`。

### C3 `SEQ4_STATUS_LEASE_MOVED`

**目标**：11 §11.4 / §11.9——属主变更后一个租约期内提示受影响客户端去查 `fs_locations`。

**改动点**

- `StateMgr::release_fsid(F)`（A3）给在 F 内持有过状态的每个客户端记
  `lease_moved_until = now + lease`；`SEQUENCE` 应答（`state_mgr.cpp:628` 置
  `status_flags` 处）若 `now < lease_moved_until` → `status_flags |= SEQ4_STATUS_LEASE_MOVED
  (0x80)`。
- 客户端在 F 之外的其他 fsid 上继续正常工作，只是多一个提示位；租约期后自动清零。

**测试**：`tests/test_state.cpp` 加 `StateMgr.LeaseMovedFlagAfterReleaseFsid`——客户端在 F1、F2 各
有 open；`release_fsid(F1)` 后 SEQUENCE 带 0x80，F2 的 IO 正常；把时钟推过 lease 后位清零；
未在 F1 持状态的第二个客户端不带该位。

### C4 ctl 与指标

**目标**：11 §11.13 的指标；运维能看每 fsid 的角色、能手动接管 / 释放。

**改动点**

- `src/server/ctl.cpp`（`ctl.cpp:518` 的 `cluster` 分支）：`deps.cluster` 改为一个变体
  （09 控制器 / `FsClusterController`），多活时：
  - `cluster status`：整体行（node、node epoch、peers）+ 每 fsid 一行
    `fsid role owner address fs_epoch fence_age grace_remaining takeovers`；`--json` 为数组；
  - `cluster takeover <fsid> [--force]`、`cluster standby <fsid>`（释放 F，走 Draining）；
  - 09 的无参 `takeover/standby` 在多活下回 `cluster: fsid required`。
- `tools/lightnfs_ctl.cpp`（`:110` 的用法串）加新形态。
- 指标（`FsClusterController::append_metrics`，与 09 C4 同一注册方式，`cluster_controller.cpp:286`
  旁）：`lightnfs_cluster_fs_role{fsid,role}`（one-hot）、`lightnfs_cluster_fs_owner{fsid,node}`
  （本网关视图；无属主时不出样本）、`lightnfs_cluster_fs_epoch{fsid}`、
  `lightnfs_cluster_fs_takeovers_total{fsid}`、`lightnfs_cluster_fs_fence_lost_total{fsid}`、
  `lightnfs_cluster_node_epoch`；引擎侧 `lightnfs_v4_moved_total{fsid}`（B3 的计数，经
  `metrics_providers`）。`lightnfs_cluster_migrations_total` 留给 D1。

**测试**：`tests/test_ctl.cpp` 仿 `Ctl.ClusterCommands`（`:528`）加 `Ctl.ClusterFsCommands`；
`tests/test_metrics.cpp` 仿 `Metrics.ClusterSeriesRenderWithControllerLifetime`（`:119`）加
`Metrics.ClusterFsSeries`（三 fsid、两个 Active 一个 Remote 的渲染；无属主不出 owner 样本）。

---

## 阶段 D：计划内迁移

### D1 `lightnfs-ctl cluster migrate <fsid> <node>`

**目标**：11 §11.7 的五步，源端发起、目标端经 store 接手，全程无网关间 RPC。

**改动点**

- `FsClusterController::request_migrate(F, T)`（在**当前属主**上执行；不是属主 → EPERM
  并提示属主是谁）：
  1. 校验 T 在 `nodes/<T>` 里（活着：`fence.<T>` 未过期），否则 EHOSTDOWN；
  2. 视图置 F = `Draining`（引擎开始对 F 回 MOVED，B3；此时 `fs_locations` 仍指自己——
     下一步立刻改）；
  3. `put_owner(F, {T, T 的地址, fs_epoch})`（先写 owner 再放围栏，11 §11.7 第 2 步）；
  4. `post(deactivate_fs(F))`：`StateMgr::release_fsid(F)`（C3 的 LEASE_MOVED 随之置位）；
  5. `release_fs_fence(F)`；视图置 F = `Remote`（owner = T）；`migrations_total++`。
- 目标端：`tick()` 的 Remote 分支加一条优先规则：`read_owner(F).node == self` 且围栏空闲 →
  **无视顺位立即接管**（走 C2 第 3 步；`prev_node` = 迁出方，其 takeover 钩子对 CephFS 是
  空回收，无害）。若 T 在 `2×ttl` 内没接（进程刚好死了），F 退回 C2 的普通无属主规则。
- `ctl.cpp`：`cluster migrate <fsid> <node>`；`lightnfs_ctl.cpp` 用法。
- `[cluster] takeover_hook` 环境变量加 `LNFS_REASON = takeover | migrate`。

**测试**：`tests/test_cluster_controller.cpp` 加 `FsClusterController.MigrateHandsOverViaOwnerRecord`
——两个控制器共用 `MemClusterStore`：gw1 `request_migrate(F, gw2)` 后 store 日志顺序为
`put_owner → release_fence`，gw1 视图 F=Remote(owner gw2)；gw2 下一次 tick 接管（不等顺位），
`activate_fs(F)` 被调；`MigrateToDeadTargetFallsBack`：gw2 记录过期 → gw1 EHOSTDOWN，不动 F。

### D2 滚动升级脚本

**目标**：11 §11.7"滚动升级一台网关"的运维封装。

**改动点**

- 新文件 `scripts/cluster_roll.sh`：

  ```
  cluster_roll.sh evacuate <node> [--to <node>]   # 对 <node> 属主的每个 fsid：migrate 到
                                                   # --to，或该 fsid nodes 里的下一个活节点；
                                                   # 轮询 cluster status 直到 <node> 无 Active
  cluster_roll.sh restore  <node>                  # 把 nodes[0] == <node> 的 fsid 迁回
  ```

  经 `lightnfs-ctl -s <socket> cluster status --json` 取 fsid 表；只用 `--json` 输出解析
  （`jq` 或 python3 单行）；每步失败即停、打印当前属主表。
- `docs/deployment.md` 新增小节引用（E3）。

**测试**：E1 脚本的 `roll` 段调用 `evacuate gw2` → 断言 gw2 无 Active、客户端不中断；`restore`
回来。`bash -n` 进 `scripts/ci.sh`。

---

## 阶段 E：验证与文档

### E1 `v4moved` 验收模式 + 三实例本机脚本

**目标**：无 root 的端到端证明，覆盖 11 §11.13 P5 的全部条目。

**改动点**

- `tests/accept_client.cpp` 加 `cmd_v4moved(host, port_a, port_b, port_c, …)`（仿
  `cmd_v4failover`，`accept_client.cpp:2313`）：
  1. **referral**：连 A，`EXCHANGE_ID` 断言 `eir_flags` 含 REFER|MIGR；`PUTROOTFH LOOKUP
     export LOOKUP b GETATTR(fs_locations, fsid)` 得到 B 的地址与 `fs_root`；同 fh
     `GETATTR(size)` → MOVED；连 B，`EXCHANGE_ID` 的 major_id ≠ A 的、scope 相同；在 B 上
     对 b 做 OPEN + LOCK + UNSTABLE 写。
  2. **migration**：`lightnfs-ctl -s B cluster migrate 2 C`；在 B 上下一次 SEQUENCE 带
     LEASE_MOVED、对 b 的 READ → MOVED、`fs_locations` → C；连 C，`OPEN(CLAIM_PREVIOUS)` +
     `LOCK(reclaim)`（DELAY 重试）、写验证器变化 → 重发、字节校验；平凡 OPEN 在 grace 内 →
     GRACE；`RECLAIM_COMPLETE` 后提前出 grace。
  3. **猝死分散接管**：`kill -9 C`（C 此时持 fsid 2、3，`nodes` 分别 `[C,A,B]` / `[C,B,A]`）；
     轮询 A/B 的 `cluster status` 直到 2→A、3→B；在 A 上 reclaim fsid 2、B 上 reclaim fsid 3；
     v3 `wtest` 对每个属主直连各跑一遍（11 §11.11 的 v3 边界：直连属主）。
  4. **单网关退化**：只起 A、三个导出 `nodes = ["A"]`，跑现有 `v4rw` + `wtest`，与单网关
     结果一致；`cluster status` 三行 Active，`fence.A` 一个文件。
- 新文件 `scripts/accept_active_active_local.sh`（仿 `accept_failover_local.sh`）：三个 lightnfsd
  共享一个 backing tree + 一个 `shared_dir`（都在 `build/` 下，见 09 的 tmpfs 句柄注意），
  各自端口即 `node_address = 127.0.0.1:<port>`；`mode = active-active`、local 后端 +
  `unsafe_skip_backend_checks`、`fence_lease = 1s`、`lease = 3s`；Release 与 ASAN 各跑一轮；
  段落：`status` → `v4moved` 四段 → `roll`（D2）→ `logs`（无 `level=error`、ASAN 干净、三个
  守护进程干净退出且 `fence.<node>` 记录列表为空集）。
- `scripts/accept_failover_vm.sh` 加 `LNFS_MODE=active-active` 轮（root VM，人工；内核客户端
  `mount -o vers=4.1` 入口地址后 `ls /mnt/export/b` 触发子挂载，`cat /proc/self/mountinfo`
  断言子挂载目标为 B 的地址；`migrate` 中跑 fsx 不中断）。**本机无 root，只 `bash -n`**。

**验收**：`accept_active_active_local.sh` Release + ASAN 全过；`accept_failover_local.sh`、
`accept_m6_local.sh`、ctest 不变。

### E2 fake 演练

- `tests/test_cephfs.cpp`：同进程两个 CephFS 导出各自 uuid；对 fsid 2 `takeover()` 后
  `cephapi_fake` 记录只有 `<cluster>-2` 被 `ceph_start_reclaim(RESET)`，fsid 1 的会话未动
  （11 §11.6 "只回收该 fsid"）；fake 注入"旧会话残留锁" → F2 的 `LOCK(reclaim)` 在 grace 内
  DELAY → 释放后成功（复用 `reclaim_probe.hpp`），F1 的锁全程不受影响。
- `tests/test_gluster.cpp` / `test_lustre.cpp`：A1 的同卷校验用真实的 `[export.gluster] volume`
  / `[export.lustre] mount` 键位走一遍（配置层测试，不需要 fake 行为）。
- 脑裂：`FsClusterController` 测试里把 `fence.gw2` 手改为列出 F1（gw1 持有）→ gw1 下一 tick
  对 F1 Draining、`fence_lost{fsid=1}=1`，F2 不受影响（同 09 E2 的围栏改写演练，scope 到 fsid）。

### E3 文档

- `docs/design/08-config-observability.md`：§8.1 加 `mode` / `node_address` / `[[export]] nodes`；
  §8.3 加 `lightnfs_cluster_fs_*`、`lightnfs_v4_moved_total`、`lightnfs_cluster_migrations_total`；
  §8.6 ctl 加 `cluster status`（多活表）/ `takeover <fsid>` / `standby <fsid>` / `migrate`。
- `docs/deployment.md`：新 §"多网关多活"——入口地址（DNS-RR / 轻 VIP 只做伪根初次接触）、
  每网关自有地址必须可达、Linux 客户端要求（v4.1 + referral）、v3 与老 v4 客户端直连属主、
  滚动升级用 `cluster_roll.sh`、Gluster/Lustre 同卷同进退；§5 主备加"二选一"说明。
- `docs/design/07-state-management.md` §7.5：grace / 名单加 fsid 维度、名单写点在多活下的变化。
- `docs/design/09-multi-gateway-failover.md` §9.9 指向 11/12 册；`11` 册状态改"实现中/已实现"
  并在 §11.13 表格标注完成日期；`docs/design/README.md` 目录加 12 册。
- 完成后与 10 册同样处理：把未闭环项收口到 `docs/toto/multi-gateway-active-active-followups.md`，
  本册撤下、记录留 git 历史。

## 12.2 每步的通用验收清单

- [ ] `mode = failover` 与单网关：ctest、`accept_m6_local.sh`、`accept_failover_local.sh` 原样通过
- [ ] 新增代码的 `scripts/format_check.sh` / `scripts/tidy.sh` 无**新增**问题（全仓库既有漂移见
      `docs/toto/multi-gateway-failover-followups.md` §3，不在本特性里顺手改）
- [ ] ASAN 配置下相关 ctest 通过
- [ ] 步骤对应的 11 册章节在 PR 描述里引用（§ 号），本册该步标 ✅ 与日期
