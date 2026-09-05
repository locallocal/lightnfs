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

[cluster]                    # 多网关主备（09 册、10 册 A1）；默认关；不可热重载
enabled = false
id = "3f9c…-uuid"            # 所有网关相同；[A-Za-z0-9_-]{8,64}
shared_dir = "/mnt/cephfs/.lightnfs-cluster"   # 共享状态目录（09 §9.4）
node = ""                    # 本网关名；空 = 主机名
role = "auto"                # active | standby | auto
fence_lease = "3s"           # 围栏续租周期（500ms–60s）；3× 未续视为失效
takeover = "auto"            # auto | manual
takeover_hook = ""           # 可选可执行脚本，接管时在后端钩子之后运行
# unsafe_skip_backend_checks = false   # 仅测试：后端能力不达标只告警

[[export]]                   # 见 06 分册 6.7；后端子表 [export.local|gluster|lustre|cephfs]
path = "/export/data"; backend = "local"; fsid = 1
clients = ["192.168.0.0/24"]; squash = "root"; readonly = false
read_bps = "0"; write_bps = "0"; iops = 0       # per-export 令牌桶
```

rsize/wsize/dtpref 不是配置项：由后端 `FsLimits`（05 分册）推导为 FSINFO / v4 属性。
校验规则启动时全量执行（fsid 唯一、路径存在——集群后端 `virtual_path` 跳过本机 stat、网段格式、TLS 证书文件），错即拒起——配置错误绝不带病运行。
`[cluster] enabled` 时另有：`id`/`shared_dir` 非空且后者为绝对路径、`role`/`takeover` 取值合法、
`server_owner`/`server_scope` 不得显式设置（身份由 `id` 派生）、`takeover_hook` 须为可执行文件；
后端构造后再查每个导出 `kStableHandles + kByteLocks + native_locks`（`--check-config` 同样执行），
`shared_dir` 不可写只告警（`--check-config` 不写共享目录）。

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

- `lightnfs-ctl`：unix socket 管理口（全部命令支持 `--json`）——`ping`/`version`/`status`、`metrics`、`dump-errors`、`drc [flush]`、`fdcache [flush]`、`clear-poison`、`state`（客户端/会话/打开/锁 dump）、`expire-client`、`conns`/`kill-conn`、`loglevel`、`reload`、`drain`、`grace-end`；另有本地子命令 `bench echo|nullrpc|fullpath`（三层基准）。排障闭环不依赖重启。
- `lightnfs-fh`：句柄解码工具（输入 hex 句柄 → fsid/ObjId/HMAC 校验结果），配 wireshark 抓包联调。
