# 8. 配置、可观测性与安全加固

## 8.1 配置

单一 TOML 文件（`/etc/lightnfs/lightnfs.toml`），启动读取；v1 发布时不做热重载，导出表变更需重启——现状见下方实现更新。

**实现更新（2026-08-28；代码在 `server/daemon.cpp` 的
`reload_config`）**：SIGHUP / `lightnfs-ctl reload` 现在热重载非拓扑子集——日志级别、slow_request_ms、error_ring、每导出 clients CIDR
与 QoS 速率、per-client QoS。导出增删（拓扑）仍需重启，前置条件是 PseudoFs 可重建与
导出表并发保护，尚未做。`grace` 已与 `lease` 解绑（`auto` = lease）。

全部键与默认值以 `config/lightnfs.toml.example`（逐键注释）和 `core/config.hpp` 为准；摘要：

```toml
[server]
reactors        = 0          # 0 = 每核一个
offload_threads = 16         # offload_heavy_threads（0 = 1/4）、offload_queue_cap = 4096
ring            = "auto"     # auto | uring | epoll；ring_sqpoll = false
bind            = ""         # 监听地址字面量；空 = 双栈全接口
port            = 2049
mount_port      = 20048
rpcbind         = true       # 向系统 rpcbind 注册（纯 v4 环境可关）；builtin_portmap 为占位键、无实现
state_dir       = "/var/lib/lightnfs"
state_shards    = 16         # v4 状态表分片数
max_connections = 4096
per_peer_limit  = 128
max_request_size = "1MiB"    # 默认 1MiB + 64KiB；示例配置放宽到 2MiB
ctl_socket      = ""         # 默认 <state_dir>/ctl.sock
metrics_port    = 0          # Prometheus 文本端点；metrics_bind = "127.0.0.1"、metrics_allow = [CIDR]
server_owner    = ""         # RFC 8881 server_owner/scope；空 = 由主机名 + state_dir 派生
log_level       = "info"     # log_file（空 = stderr）、log_rotate_size = "50MiB"、log_rotate_keep = 5
slow_request_ms = 1000       # 慢请求日志阈值；error_ring = 64（dump-errors 采样环）

[protocol]
v3       = true              # 解析但不可关闭（v3 恒开）
v4       = true              # minorversion 1/2；4.0 恒拒绝
delegations = true           # 读委托开关
lease    = "90s"
grace    = "auto"            # 重启后 reclaim 窗口；auto = lease
courtesy_multiplier = 24
drc_ttl  = "120s"
drc_mem  = "64MiB"

[limits]
inflight_per_conn = 64       # v3；v4 由会话槽数（32 槽 × 8KiB 缓存）控制，不可配
client_read_bps = "0"; client_write_bps = "0"; client_iops = 0   # per-clientid 令牌桶

[tls]                        # RPC-over-TLS（RFC 9289），需 OpenSSL 构建
mode = "off"                 # off | optional | required；cert/key/ca/client_cert

[cluster]                    # 多网关主备（09 册）/ 多活（10 册）；默认关；不可热重载；mode 二选一
enabled = false
id = "3f9c…-uuid"            # 所有网关相同；[A-Za-z0-9_-]{8,64}
shared_dir = "/mnt/cephfs/.lightnfs-cluster"   # 共享状态目录（09 §9.4；多活的 per-fsid 键空间见 10 §10.3）
node = ""                    # 本网关名；空 = 主机名
role = "auto"                # active | standby | auto；多活下只能 auto（角色按导出，不按进程）
fence_lease = "3s"           # 围栏续租周期（500ms–60s）；3× 未续视为失效；多活下一个网关的所有 fsid 一条批量续租
takeover = "auto"            # auto | manual；多活下按导出生效：manual = 只认 `ctl cluster takeover <fsid>` / `migrate`
takeover_hook = ""           # 可选可执行脚本，接管时在后端钩子之后运行（超时 fence_lease；环境变量 LNFS_CLUSTER_ID/NODE/EPOCH/PREV_NODE；多活另有 LNFS_FSID、LNFS_REASON=takeover|migrate）
mode = "failover"            # failover（09 主备，默认）| active-active（10 册多活，每导出一个属主网关）
node_address = ""            # 多活必填：本网关自有地址 "host:port" / "[v6]:port"，写入 owner 记录、作为 fs_locations 指向本网关的地址；failover 下忽略
exports_source = "local"     # local（本文件的 [[export]]，默认）| catalog（共享目录 catalog.toml，11 册；本文件不得再有 [[export]]，首版发布前可空表启动；改动需重启）
catalog_refresh = "auto"     # 仅 catalog：auto = 围栏 tick 上发现新版即应用 | manual = 只记待应用，`ctl cluster catalog apply` / reload / SIGHUP 触发；可热改
# unsafe_skip_backend_checks = false   # 仅测试：后端能力不达标只告警

[backend_defaults.cephfs]    # 仅 catalog：本机键（conf/keyring/id/user/name/log_file/fd_cache/mon_host）合并进清单里每个该后端的导出；出现集群键报错；local 模式下告警忽略
conf = "/etc/ceph/ceph.conf"; keyring = "/etc/ceph/ceph.client.gw1.keyring"

[[export]]                   # 见 06 分册 6.7；后端子表 [export.local|gluster|lustre|cephfs]
# [export.cephfs] uuid = ""  # 多网关接管回收的会话 uuid（09 实施步骤 D2）；空 = <cluster id>-<fsid>，各网关相同
path = "/export/data"; backend = "local"; fsid = 1
clients = ["192.168.0.0/24"]; squash = "root"; readonly = false
# nodes = ["gw1", "gw2"]    # 多活属主优先级列表（10 §10.3）：仅 mode = active-active 且每导出必填；第一个活着的网关服务、其余按序接管；进导出摘要（全集群逐字相同）；改动需重启
read_bps = "0"; write_bps = "0"; iops = 0       # per-export 令牌桶
```

rsize/wsize/dtpref 不是配置项：由后端 `FsLimits`（05 分册）推导为 FSINFO / v4 属性。
`[[export]]` 块只认上面列出的键，未知键报错（12 册 A2 起；此前静默忽略）；`disabled` 是清单（11 册）专用键，本地文件里出现同样报错。
校验规则启动时全量执行（fsid 唯一、路径存在——集群后端 `virtual_path` 跳过本机 stat、网段格式、TLS 证书文件），错即拒起——配置错误绝不带病运行。
`[cluster] enabled` 时另有：`id`/`shared_dir` 非空且后者为绝对路径、`role`/`takeover` 取值合法、
`server_owner`/`server_scope` 不得显式设置（身份由 `id` 派生）、`takeover_hook` 须为可执行文件；
后端构造后再查每个导出 `kStableHandles + kByteLocks + native_locks`（`--check-config` 同样执行），
`shared_dir` 不可写只告警（`--check-config` 不写共享目录）。
`mode = "active-active"` 时再加（10 §10.10，`core/config.cpp` 的 `validate_active_active`）：`node_address`
形如 `host:port`（不解析 DNS）、`role` 只能 `auto`、`takeover` 取值不变、每个 `[[export]]` 的 `nodes`
非空且无重复（名字 `[A-Za-z0-9_.-]{1,64}`；成员是否为集群里的活网关在运行期按 `fence.<node>` 心跳判定）、
同一 Gluster `volume` / Lustre `mount` 上的导出 `nodes` 必须逐项相同（10 §10.6 同卷同进退：libgfapi
连接 / Lustre 挂载是整卷级，单 fsid 迁移会波及同卷其他 fsid）；failover 下出现这些键只告警忽略
（便于逐台切换配置）。校验失败的原因以 WARN 日志给出，`validate_config` 只返回 EINVAL。
启动时还把导出表的规范化摘要（`sha256:` + 每导出的 path/fsid/backend/readonly/squash/anon_uid/anon_gid
与后端子表键值、多活下再加 `nodes` 一行，按 fsid 排序）写入 `shared_dir/exports.<node>`，与其他节点的记录逐一比对，
不一致则拒绝入集群。**按节点豁免键**不参与摘要：`conf`、`keyring`、`id`、`user`、`name`、
`log_file`、`fd_cache`、`mon_host`（`core/config.hpp` 的 `kPerNodeBackendKeys`）；`clients`
与 QoS 也不参与（可热重载的策略，不是树身份）。已下线节点的 `exports.<node>` 需运维手动删除。

## 8.2 日志

- 结构化（logfmt），全异步（有界队列 → 落盘线程），热路径日志零分配；输出到 stderr 或按大小轮转的文件（`log_file`/`log_rotate_size`/`log_rotate_keep`）。
  **实现偏差（2026-08-23）**：落地为 spdlog（third_party/spdlog 子模块）异步 logger——
  有界队列 + 单落盘线程 + 满则丢弃新条目（不阻塞 reactor），`util/log.hpp` 门面与
  logfmt 输出格式不变；"零分配"弱化为 fmt 栈内联缓冲（超长消息堆分配）。
- 级别约定：`error`=数据/协议正确性风险（fsync 失败、白名单外错误映射）；`warn`=可疑客户端行为（BADHANDLE、SEQ_MISORDERED）；`info`=生命周期（挂载、grace、回收）；`debug`=每请求单行摘要。
- **每请求摘要行**（debug，v4 调试第一生产力，nfsv4/11.6）：
  `xid=… peer=… v4 tag="…" ops=[SEQUENCE,PUTFH,OPEN,GETFH,GETATTR] st=OK dur=1.2ms`
- 采样机制：`debug` 关闭时对错误应答自动采样保留最近 N 条完整摘要（环形），`lightnfs-ctl dump-errors` 取出——生产排障不必开全量 debug。

## 8.3 指标（Prometheus 文本口）

| 组 | 指标示例 |
|----|----------|
| rpc | 每程序/过程/操作计数与时延直方图（v4 按 COMPOUND 内 op 展开）、错误码计数 |
| transport | 连接数（accepted/active/rejected）、背压等待次数、buffer 池水位；收发字节在 per-export 计数 |
| runtime | reactor 循环延迟、offload 队列深度/等待时延、buffer 池水位 |
| state | 07 分册 7.8 清单 |
| backend | 计数/水位而非直方图：`lightnfs_fdcache_*`（local/lustre）、`lightnfs_gluster_*`、`lightnfs_cephfs_*`（含 `_blocklisted_total`）、`lightnfs_lustre_hsm_*`，各带 jukebox 计数与锁描述符数；时延直方图只在协议层（v3 过程 / v4 op / COMPOUND / reactor 循环） |
| drc/slots | 命中/重放/in-progress 等待 |
| cluster | 多网关主备（09 册，`mode = failover`）：`lightnfs_cluster_role{role}`（one-hot）、`_epoch`、`_fence_owned`、`_fence_age_seconds`（自己持有 = 距上次续租，否则距读到他人记录；未见记录不出样本）、`_takeovers_total`、`_fence_lost_total`、`_activation_failures_total`、`_activation_seconds` 直方图（§9.6 "< 1s" 目标）；由控制器注册、随进程存活，standby 期间也可见 |
| cluster（清单） | `exports_source = "catalog"`（11 册 §11.8，12 册 C3，`CatalogApplier::append_metrics`，随应用器注册、主备与多活同一组）：`lightnfs_cluster_catalog_version`（已应用版本，无清单为 0）、`lightnfs_cluster_catalog_latest_version`（共享目录里最近一次看到的版本；清单消失为 0）、`lightnfs_cluster_catalog_pending`（0/1：看到了新版还没应用——manual 模式待运维 `cluster catalog apply`，或上次应用失败）、`lightnfs_cluster_catalog_applies_total`（换版成功次数）、`lightnfs_cluster_catalog_apply_failures_total`；告警：`catalog_version` 落后 `catalog_latest_version` 超过 N 个 lease、`apply_failures_total` 增长、各网关 `catalog_version` 不一致 |
| cluster（多活） | `mode = active-active`（10 §10.13，`FsClusterController::append_metrics`）：整机两条 `lightnfs_cluster_node_epoch`（本网关自有 epoch，进程启动 +1）、`lightnfs_cluster_migrations_total`（本网关作为源发起的 `cluster migrate` 次数）；每导出一组带 `{fsid}` 标签——`lightnfs_cluster_fs_role{fsid,role}`（one-hot，`role` ∈ active / activating / draining / remote / unowned，本网关视角）、`lightnfs_cluster_fs_owner{fsid,node}`（本网关看到的属主，值恒 1；无属主时不出样本，可据此告警"导出无人服务"）、`lightnfs_cluster_fs_epoch{fsid}`（该导出的接管代数，只做诊断，不进 stateid）、`lightnfs_cluster_fs_takeovers_total{fsid}`、`lightnfs_cluster_fs_fence_lost_total{fsid}`（该导出的围栏被他人改写 → Draining）、`lightnfs_cluster_fs_activation_failures_total{fsid}`；引擎侧 `lightnfs_v4_moved_total{fsid}`（`server/metrics_providers.cpp` 的 `append_v4_moved`：非属主导出边界回 `NFS4ERR_MOVED` 与缺席 fs 属性应答的计数，持续增长说明客户端没有跟随 `fs_locations`）。09 的 `lightnfs_cluster_role/_epoch/_fence_*` 整机系列在多活下**不**出样本 |

SLI 建议：READ/WRITE p99、GETATTR p99、错误率、grace 时长。

**实现（2026-08-28）**：时延直方图为固定桶（100µs–5s，Prometheus
histogram 语义）；v4 按 op 展开 calls/errors/duration，另有整 COMPOUND 直方图；
per-export 维度落地为带 `{export,fsid}` 标签的数据面计数（read/write bytes+ops、
fd 缓存）；runtime 组含 offload 队列深度、buffer 池水位、reactor 循环忙时直方图。
分工：引擎热路径上的计数与直方图在 `obs/metrics.{hpp,cpp}`（含文本提供者注册表）；
读取其他子系统统计的提供者——DRC、v4 状态表、每导出数据面 + 各后端缓存/jukebox/锁句柄、
runtime 的 offload 池与 reactor 循环——在 `server/metrics_providers.{hpp,cpp}`
（`register_metrics_providers(MetricsSources)`），由 `main.cpp` 在协议栈装配后调用一次。

## 8.4 追踪

原设想 `OpCtx.trace` 贯穿请求 → core → 后端逐段打点；实现为引擎内的 span 记录（无独立追踪类型）+ 慢请求（>阈值）自动落日志；OTLP 导出未做。

**实现（2026-08-28）**：span 粒度为 v4 COMPOUND 内逐 op（前 32 个 op 的耗时记录在
请求上下文里，零分配）、v3 为单过程；超过 `[server] slow_request_ms`（默认 1000ms，
0 关闭）时 warn 日志附耗时分解。后端级逐段打点与 OTLP 仍留待后续。

## 8.5 安全加固清单（实现验收项）

汇总各分册红线，作为发布前 checklist：

1. 记录/字段长度上限逐处校验（03 分册 3.2/3.6），fuzz 目标覆盖 `handle_request` 全入口（nfsv3/09 §9.8）；
2. 句柄 HMAC + NFS 层每请求导出/IP 校验（04 分册 4.3，nfsv3/09 §9.6）；
3. 名字校验：空名、`/`、NUL、`.`/`..`（v4 BADNAME）双层（core+后端 O_NOFOLLOW）；
4. squash 在 auth 层一次完成（03 分册 3.5），后端不见原始 root；
5. 宽限期 reclaim 名单强制（07 分册 7.5）；
6. 资源上限全部有默认值且可配（连接、在途、DRC 内存、槽缓存、fd 缓存、buffer 池）；
7. 以最小特权运行：CAP_DAC_READ_SEARCH（open_by_handle_at）+ CAP_NET_BIND_SERVICE，其余全 drop；systemd 单元带 seccomp 白名单（uring + 文件系统调用集）；
8. AUTH_SYS 的信任边界写进部署文档：仅受信网络，公网部署必须前置 TLS/WireGuard。

## 8.6 工具

- `lightnfs-ctl`：unix socket 管理口（全部命令支持 `--json`）——`ping`/`version`/`status`、`metrics`、`dump-errors`、`drc [flush]`、`fdcache [flush]`、`clear-poison`、`state`（客户端/会话/打开/锁 dump）、`expire-client`、`conns`/`kill-conn`、`loglevel`、`reload`、`drain`、`grace-end`、`cluster status|exports [<node>]|takeover [--force]|standby|catalog …|export …`（多网关主备角色：查看角色/epoch/围栏/同伴，手动接管或退回 standby，09 §9.10 C3；单网关答 `cluster: not enabled`）；另有本地子命令 `bench echo|nullrpc|fullpath`（三层基准）。排障闭环不依赖重启。
  - **多活（`mode = active-active`，10 §10.7 / §10.13；`server/ctl.cpp` 的 `cluster_fs_status` 与 `cluster` 分支）**：同一 `cluster` 子命令换成按导出的形态——
    `cluster status` 先一行网关总览 `mode=active-active node= node_epoch= node_address= shared_dir= peers= peers_alive= takeover= migrations= exports=`（`peers` = 共享目录里登记过地址的网关，`peers_alive` = `fence.<node>` 心跳未过期的网关；`exports_source = "catalog"` 时行尾再接 `catalog=<已应用版本|none> catalog_latest=<共享目录里最近看到的版本|none> catalog_refresh=auto|manual catalog_error=-|<上次应用失败原因，自由文本，恒在行尾>`，主备形态的 `cluster status` 行尾同样四个字段；本地模式不出现，12 册 C3），再每导出一行
    `fsid= role= nodes= owner= address= fs_epoch= fence_age_ms= fence_expires_in_ms= grace_remaining_s= takeovers= fence_lost= activation_failures=`（`role` 为本网关视角的 active/activating/draining/remote/unowned；无属主时 `owner=none address=-`，无围栏记录时两个 fence 字段为 `-`；`grace_remaining_s` 只对本网关 Active 的导出有意义）；`--json` 同样字段，`exports` 为数组。
    `cluster exports [<node>]`：列出一个网关（默认本网关）此刻服务的导出——一行 `node= alive= address= exports= fsids=`（`alive` 按 `fence.<node>` 心跳，`address` 取本机配置 / `nodes/<node>` 登记 / owner 记录），再每导出一行 `fsid= path= role= fs_epoch=`（`role` 仍是本网关视角：查自己时 active / activating / draining，查别人时 remote）；`--json` 为 `{"node","alive","address","fsids":[…],"exports":[{fsid,path,role,fs_epoch}]}`。节点既未登记、也不在任何导出的 `nodes` 里时报 `unknown node`。这就是 v3 客户端"每导出挂到属主地址"所需的导出 → 属主映射（10 §10.11）。
    `cluster takeover <fsid> [--force]`：对本网关视角为 remote 的导出取围栏并接管（围栏仍有效时报 `fence held by <node>`，`--force` 覆盖——仅在确认对方已死时用）；`cluster standby <fsid>`：释放本网关持有的一个导出（走 Draining，释放后不再自动抢回，直到别的网关持有过它或运维再次 `takeover`）；`cluster migrate <fsid> <node>`：**在当前属主上运行**，把一个导出交给一个活着的同伴（先写 owner 记录再放围栏，目标网关的下一次轮询无视顺位接管；非属主上执行报 `not active here (role=…, owner=…)`，目标无心跳报 `not a live gateway`）。09 的无参 `takeover` / `standby` 在多活下答 `fsid required`。滚动维护的封装见 `scripts/cluster_roll.sh`（10 §10.7）。
  - **共享导出清单（`exports_source = "catalog"`，11 册 §11.10 / 12 册 D1；`server/ctl_catalog.cpp`）**：`cluster catalog show`（头行 `version= exports= updated_at= updated_by= comment=`，再每导出一行 `fsid= path= backend= nodes= disabled= clients= readonly= squash= anon_uid= anon_gid= read_bps= write_bps= iops= keys=`；无清单答 `catalog: none`）、`cluster catalog status`（每网关一行 `node= applied= alive= applied_at= digest= status=`，末行 `latest=`）、`cluster catalog history`（每版一行同头行字段 + `current=yes|no`）、`cluster catalog diff [<v1>] [<v2>]`（`from= to=` 后七类各一行：added / removed / disabled / enabled / nodes_changed / dynamic_changed / rejected；`0` 或 `none` 表示首发前的空清单，缺省 `v1` 为本网关已应用版、`v2` 为当前版）、`cluster catalog import <网关侧文件> [--dry-run] [--comment=TEXT]`（本地配置文件或清单文档整体替换清单：剥本机键、集群级校验、同 fsid 改 path / backend / 集群键被拒、CAS 提交并在被人抢先时重读重试 3 次；`--dry-run` 只报 diff）、`cluster catalog rollback <version> [--comment=TEXT]`（把历史某版作为新版提交）、`cluster catalog apply`（12 册 C2）。只要 `[cluster] enabled`，本地模式也能用（先 `import` 再切换，11 §11.9）；单网关答 `cluster: not enabled`。审计头 `updated_by = "<node> uid=<SO_PEERCRED>"`。单导出编辑（12 册 D2，同文件）：`cluster export list`（= `show` 的导出行）、`cluster export add --path P --fsid N [--backend B] [--nodes a,b] [--clients c1,c2] [--readonly] [--squash root|all|none] [--anon-uid N] [--anon-gid N] [--read-bps N] [--write-bps N] [--iops N] [--opt k=v …] [--disabled] [--force] [--dry-run] [--comment T]`（`--opt` 是后端集群键，本机键被集群级校验拒；fsid 复用规则：历史里同 fsid 曾是别的 path / backend / 集群键 → 拒绝，`--force` 覆盖）、`cluster export set <fsid> …`（同一组在线可变标志，`--readonly=false` / `--disabled=false` 清除；`--path` / `--backend` / `--opt` 被拒："remove and re-add, or use a new fsid"）、`cluster export remove <fsid> [--force]`（本网关视角有属主——多活看 owner view，主备看本机 Active——则拒绝 "fsid N is served by gw2 (disable it first, or --force)"）；标志 `--k=v` 与 `--k v` 都收；三者回答 `catalog vN committed (was vM): export fsid=X added|updated|removed: <diff>`，`--dry-run` 只报不写。提交在 CAS 重试时**重读当前版再施加变更**，并发的单导出编辑不互相覆盖。ctl 客户端把 `cluster export …` 整行原样转发（不经 ccmd 标志解析），由服务端解析与回 usage。线协议（12 册 D1）：参数按空白切分，`"…"` 内空白原样、`\"` / `\\` 转义，ctl 客户端对含空白 / 引号 / 反斜杠的参数自动加引号；裸 `--json` 在任意位置选 JSON 渲染。
- `lightnfs-fh`：句柄解码工具（输入 hex 句柄 → fsid/ObjId/HMAC 校验结果），配 wireshark 抓包联调。
