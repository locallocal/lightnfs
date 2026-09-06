# 部署与运维

lightnfs 是一个用户态 NFS 网关（NFSv3 + NFSv4.1/4.2，读写），面向"受信网络内导出本地目录树
或集群文件系统（GlusterFS / Lustre / CephFS）"的场景。本文是发布运维的落地指南：信任边界、
最小特权部署、配置要点、可观测性与已知限制。

## 1. 安全信任边界（务必先读）

**lightnfs 只支持 AUTH_SYS（含 AUTH_NONE）鉴权。** AUTH_SYS 的 uid/gid 由客户端自行声明，
服务器无法验证——**这等同于"网络内任意主机可声称任意用户身份"**。因此：

- **只在受信网络部署**（专用存储 VLAN、容器内网、回环）。**严禁裸露公网。**
- 公网/跨信任域访问**必须**加密。两条路：(a) 内置 **RPC-over-TLS**（RFC 9289，见下）——
  客户端以 `xprtsec=tls` 挂载即触发 STARTTLS，传输层加密 + 服务器证书认证；(b) 前置
  WireGuard / IPsec 隧道，或 stunnel 终止后转发到 2049。lightnfs 自身仍不做
  RPCSEC_GSS/krb5（设计取舍 D8）——**TLS 保通道、AUTH_SYS 报身份**：TLS 加密并认证信道，
  但用户身份仍由客户端 AUTH_SYS 声明，故上面的受信网络假设不因 TLS 而放宽。
- **RPC-over-TLS 配置**（`[tls]` 段，需构建时带 OpenSSL）：`mode = "off"`（默认）/
  `"optional"`（宣告 STARTTLS，同时仍服务明文客户端）/`"required"`（宣告 STARTTLS 并拒绝
  明文 NFS/MOUNT 操作，回 AUTH_TOOWEAK，仅放行 NULL 探测/健康检查）；`cert`/`key` 为服务器
  PEM 证书链与私钥；可选 `ca` + `client_cert = true` 启用双向 TLS（校验客户端证书）。
  改动 `[tls]` 需重启（非热重载项）。
- 用 `[[export]] clients = [...]` CIDR 白名单收敛来源，用 `squash` 把不受信客户端的
  root 映射为匿名（默认 `root`；完全不信任时用 `all`）。
- 句柄经 SipHash-2-4 HMAC 签名（`state_dir/hmac.key`，首启生成，0600），伪造句柄→
  BADHANDLE；每请求还校验导出 fsid 与来源 IP。**但这防的是伪造句柄，不是伪造身份**——
  身份边界仍是上面的网络假设。

## 2. 最小特权部署（systemd）

`packaging/systemd/lightnfs.service` 是按安全清单 §8.5 第 7 项写的最小特权单元：

- 专用系统用户 `lightnfs`（对导出树只读/按需读写，自身不拥有系统文件）；
- 仅两个 capability：`CAP_DAC_READ_SEARCH`（`open_by_handle_at` 稳定句柄，设计 06；只导出
  gluster/lustre/cephfs 时不需要）与 `CAP_NET_BIND_SERVICE`（绑定 2049/20048），其余
  `CapabilityBoundingSet` 清空、`NoNewPrivileges`；
- 文件系统沙箱：`ProtectSystem=strict` + 仅 `state_dir` 与显式列出的导出树可写；
- seccomp 白名单：`@system-service` 去掉高危集，再显式加入 io_uring 与句柄系统调用
  （允许集由 `scripts/gen_seccomp_allowlist.sh` 从真实 v3+v4.1 读写+锁+v4.2 稀疏/拷贝负载
  的 strace 生成，运行时若变更需复核；v4.2 用到的 `lseek`/`fallocate`/`copy_file_range`/
  `ioctl(FICLONERANGE)` 与探测用 `openat(O_TMPFILE)` 均在 `@system-service` 内，无需额外放行）。

打包安装：`packaging/make_tarball.sh` 产出
`packaging/dist/lightnfs-<ver>-linux-<arch>.tar.gz`（/usr/local 布局）、
`packaging/make_deb.sh` 产出 .deb（dpkg-deb，装到 /usr 与 /etc/lightnfs）、
`packaging/make_rpm.sh` 产出 .rpm（需 rpmbuild）。三者共享同一 CMake install
文件集（bin、config 示例、systemd 单元、部署文档）。手工安装等价步骤：

```bash
sudo useradd --system --home /var/lib/lightnfs --shell /usr/sbin/nologin lightnfs
sudo install -m755 build-rel/lightnfsd /usr/local/bin/lightnfsd
sudo install -m755 build-rel/lightnfs-ctl /usr/local/bin/lightnfs-ctl
sudo install -Dm644 config/lightnfs.toml.example /etc/lightnfs/lightnfs.toml
sudoedit /etc/lightnfs/lightnfs.toml     # 填导出路径、clients CIDR、squash
sudo cp packaging/systemd/lightnfs.service /etc/systemd/system/
# 把每个导出目录树加进单元的 ReadWritePaths=（只读导出用 ReadOnlyPaths=）
sudo systemd-analyze security lightnfs.service   # 审计沙箱评分
sudo systemctl enable --now lightnfs
```

不用 systemd 时，等价地：以非 root 用户运行，仅授予上述两个 capability
（`setcap 'cap_dac_read_search,cap_net_bind_service=ep' lightnfsd`，或用高位端口免去
`CAP_NET_BIND_SERVICE`），并用容器/`bwrap` 限制可见文件系统。

## 3. 关键配置

见 `config/lightnfs.toml.example`（每键有注释）。发布前必查：

| 项 | 键 | 说明 |
|----|----|------|
| 端口 | `[server] port` / `mount_port` | 默认 2049 / 20048 |
| 状态目录 | `[server] state_dir` | 存 boot_epoch、hmac.key、grace 名单——**须持久、独占、0700** |
| 来源白名单 | `[[export]] clients` | CIDR 列表，收敛到受信网段 |
| 身份压缩 | `[[export]] squash` | `root`（默认）/`all`/`none` |
| 只读 | `[[export]] readonly` | 只读导出置 `true` |
| 后端 | `[[export]] backend` + `[export.local]` / `[export.gluster]` / `[export.lustre]` / `[export.cephfs]` | `local`（本机目录树）、`gluster`（libgfapi 卷：`volume`/`servers`/`subdir`；运行时加载 `libgfapi.so.0`，缺库启动失败并写明；`path` 只是挂载名）或 `lustre`（Lustre 客户端挂载内的目录：`mount`（默认自动探测）/`hsm`/`native_locks`/`identity`/`fd_cache`；非 Lustre 挂载启动即拒，写明 statfs magic 不符）或 `cephfs`（libcephfs 挂载：`conf`/`id`/`keyring`/`mon_host`/`fs_name`/`subdir`/`options`/`uuid`（多网关接管时回收的会话 uuid，默认 `<cluster id>-<fsid>`）；运行时加载 `libcephfs.so.2`，缺库启动失败并写明；`path` 只是挂载名） |
| 监听地址 | `[server] bind` | 监听地址字面量；空 = 全接口双栈。收敛到存储网卡 |
| 传输加密 | `[tls] mode` / `cert` / `key` / `ca` / `client_cert` | RPC-over-TLS（RFC 9289）：off/optional/required + 证书；改动需重启 |
| 租约 | `[protocol] lease` | v4.1 租约（默认 90s） |
| 宽限 | `[protocol] grace` | 重启后 reclaim 窗口；`auto`（默认）= lease，可设更短加快恢复 |
| 日志文件 | `[server] log_file` / `log_rotate_*` | 空 = stderr；否则按大小轮转的文件 |
| 限速 | `[[export]] read_bps/write_bps/iops`、`[limits] client_*` | 令牌桶；0 = 不限，热重载 |
| courtesy | `[protocol] courtesy_multiplier` | 过期客户端保留 `N×lease`（默认 24），冲突则立即回收 |
| 资源上限 | `[server]`/`[limits]` 各键 | 连接/在途/请求大小/DRC/fd 缓存均有默认且可配 |

`lightnfsd --check-config --config <file>` 只校验配置不启动。

## 4. 运维与可观测性

- **ctl 套接字**（`state_dir/ctl.sock`，或 `[server] ctl_socket`）：
  `lightnfs-ctl <cmd>`——`ping`、`version`、`status`（版本/uptime/连接数/drain/grace）、
  `metrics`（Prometheus 文本）、`dump-errors`、`drc [flush]`、`fdcache [flush]`、
  `clear-poison`、`state`（v4 状态表：clients/sessions/opens/locks 计数 + 三表 dump）、
  `expire-client <clientid>`（强制回收某客户端全部状态，排查挂死/泄漏）、
  `conns` / `kill-conn <id>`（连接列表与强制断开）、`loglevel <lv>`、`reload`
  （热重载，见下）、`drain`（停止接受新连接、存量继续服务——从 LB 优雅摘流，重启前
  不可逆）、`grace-end`（提前结束 grace）、`cluster status|takeover [--force]|standby`
  （多网关主备，09/10 册：`status` 一行给出 `role= node= epoch= fence_owner= fence_age_ms=
  shared_dir= peers=` 及接管计数；`takeover` 让 standby 网关取围栏并开始服务，`--force`
  覆盖他人仍有效的围栏——仅在确认对方已死时使用；`standby` 让 active 网关排空连接并释放围栏。
  单网关答 `cluster: not enabled`）。所有命令加 `--json` 输出机器可读 JSON。
  另有本地子命令 `lightnfs-ctl bench <echo|nullrpc|fullpath>`——三层基准（02 分册
  §2.8），自起进程内栈压测，不经 ctl 套接字、不涉运行中的服务。
- **热重载**（SIGHUP 或 `lightnfs-ctl reload`）：重新解析
  配置文件并应用非拓扑子集——日志级别、slow_request_ms、error_ring、每导出 clients
  白名单与 QoS 速率、[limits] client_*。导出增删、监听地址/端口、线程拓扑、
  state_dir/lease 等改动会在 reload 报告中标注 restart required，不会带病生效。
  systemd 单元的 `ExecReload`（`systemctl reload lightnfs`）即发 SIGHUP。
- **限速 / QoS**：per-export（`[[export]] read_bps/write_bps/iops`）与 per-client
  （v4 clientid 级，`[limits] client_read_bps/client_write_bps/client_iops`）令牌桶，
  接在引擎 READ/WRITE 入口、对象锁之前；超配额的请求被延迟（debt 模式：大于突发
  容量的单笔请求放行并透支，由后续请求偿还），不会报错。速率全部热重载。
- **Prometheus**：`[server] metrics_port` 开一个 HTTP 文本端点；关键指标包括
  `lightnfs_v4_{clients,sessions,opens,files_with_state,courtesy_clients,in_grace,
  grace_remaining_seconds,lock_states,lock_segments,lock_owners}`、
  `lightnfs_v4_reclaims_total{reason}`、`lightnfs_v4_lock_denied_total`、
  `lightnfs_drc_*`、连接/背压计数。时延类指标为固定桶直方图（可算 p99）：
  `lightnfs_v3_duration_seconds{proc}`、`lightnfs_v4_op_duration_seconds{op}`
  （另有 `lightnfs_v4_op_{calls,errors}_total{op}`）、
  `lightnfs_v4_compound_duration_seconds`、`lightnfs_reactor_loop_duration_seconds`。
  多导出定位用带 `{export,fsid}` 标签的
  `lightnfs_export_{read,write}_{bytes,ops}_total` 与 `lightnfs_fdcache_*`；
  runtime 层另有 `lightnfs_offload_*` 与 `lightnfs_buffer_pool_free_bytes{listener}`。
  多网关模式（`[cluster] enabled`）再加 `lightnfs_cluster_role{role}`（one-hot）、
  `lightnfs_cluster_epoch`、`lightnfs_cluster_fence_owned` / `_fence_age_seconds`、
  `lightnfs_cluster_{takeovers,fence_lost,activation_failures}_total` 与
  `lightnfs_cluster_activation_seconds` 直方图——告警建议：`fence_age_seconds` 超过
  `fence_lease` 的 2 倍、`fence_lost_total` 增长、`role{role="active"}` 在集群内之和 ≠ 1。
- **接管钩子**（`[cluster] takeover_hook`，10 册 D1）：接管时先对每个导出调后端的
  `takeover()`（默认空操作；CephFS 的会话回收见 D2），再以进程身份执行该脚本，环境变量
  `LNFS_CLUSTER_ID`、`LNFS_NODE`、`LNFS_EPOCH`、`LNFS_PREV_NODE`（被替换的围栏记录所属节点，
  首次启动为空）；多活（§6）下每次只接管一个导出，再加 `LNFS_FSID`（该导出的 fsid，主备接管为空）
  与 `LNFS_REASON`（`takeover` = 属主猝死/被驱逐，`migrate` = 前属主主动交出）。
  超时 `fence_lease` 后 SIGKILL；超时或非零退出只记 warn，接管照常继续
  （grace 内 reclaim 下推失败走 DELAY 重试）。Lustre 的驱逐放在这里：
  `lctl set_param mdc.*.evict_client=<$LNFS_PREV_NODE 的 NID>`（脚本自己维护节点→NID 映射）。
- **慢请求日志**：超过 `[server] slow_request_ms`（默认 1000，0 关闭）的请求落一条
  warn 日志；v4 附 COMPOUND 内逐 op 耗时分解（`ops=[PUTFH=12us,READ=890000us]`），
  现网定位的第一工具。`dump-errors` 采样环大小由 `[server] error_ring`（默认 64）控制。
- **重启恢复**：进程重启后 boot_epoch +1，读 `state_dir/clients/` 名单进入 grace
  （时长 = lease），仅名单内客户端可 reclaim；名单内全部 RECLAIM_COMPLETE 则提前结束。
  普通操作在 grace 内收 GRACE 重试。**不要清空 state_dir**，否则客户端无法 reclaim、
  可能丢未提交写。

## 5. 多网关主备（高可用）

多个 lightnfsd 网关共挂同一个共享后端（GlusterFS / Lustre / CephFS）时，`[cluster]`
段开启后一个网关故障、客户端切到另一个网关**不重挂载、不重建应用状态**：打开的文件、
字节锁、未提交的写由 NFSv4.1 的 grace/reclaim 机制恢复（设计见 09 册，实现见 10 册）。

**二选一**：同一个 `[cluster]` 段有两种互斥的形态，由 `mode` 选择——`mode = "failover"`
（默认，本节：一个 VIP、整机一个角色、一个网关服务全部导出）或 `mode = "active-active"`
（§6：每个导出一个属主网关、协议引导客户端）。一个部署只能是其中之一，`nodes` /
`node_address` 在 failover 下被忽略（只告警），`role = active|standby` 在多活下被拒绝。

**拓扑**：客户端只认一个服务地址（VIP 或 DNS 单名），地址漂移由外部 HA 完成——
lightnfsd 不搬 VIP，只负责"谁在服务"（围栏）与状态恢复（grace）。典型两节点：

```
                 ┌─────────── VIP 10.0.0.9 (keepalived) ───────────┐
   NFS client ──▶│  MASTER: gw1 (role=active)   BACKUP: gw2 (standby)│
                 └───────────────┬──────────────────┬───────────────┘
                    shared_dir （集群 FS 上，两网关都可读写）
```

**keepalived 挂钩**：VIP 归属变化时驱动接管，与 lightnfsd 的围栏互为二次防线。

```
# /etc/keepalived/keepalived.conf （gw1；gw2 priority 更低）
vrrp_instance lnfs {
  state MASTER
  interface eth0
  virtual_router_id 51
  priority 100
  virtual_ipaddress { 10.0.0.9 }
  notify_master "/usr/local/bin/lightnfs-ctl -s /var/lib/lightnfs/ctl.sock cluster takeover"
  notify_backup "/usr/local/bin/lightnfs-ctl -s /var/lib/lightnfs/ctl.sock cluster standby"
  notify_fault  "/usr/local/bin/lightnfs-ctl -s /var/lib/lightnfs/ctl.sock cluster standby"
}
```

拿到 VIP 的节点 `cluster takeover` 取围栏（epoch+1）并开始监听；失去 VIP 的节点
`cluster standby` 排空连接、释放围栏。`takeover = manual` 时只认 ctl 请求，把"谁服务"
的决策权完全交给 keepalived；`takeover = auto` 则 lightnfsd 自己在围栏过期时接管，
keepalived 只管地址漂移（两者可叠加：VIP 是第一反应，围栏是脑裂时的第二道闸）。

**部署要点**（配置校验期会拦下明显错误，其余靠运维遵守）：

- **`bind` 只绑 VIP**：standby 不监听，active 监听 VIP。若绑 `0.0.0.0`，一个刚 drain
  的旧 active 仍会应答直到端口关闭——把 `[server] bind` 设成 VIP（或用 keepalived 的
  非抢占模式 + `bind` 到本机固定地址两选一），避免两个地址同时可达。
- **NTP 必须开**：围栏租约用墙钟（`CLOCK_REALTIME`）跨节点比较，容忍 500ms 偏差
  （`kFenceSkewTolerance`）。所有网关必须同步时钟，否则围栏过期判定会漂。
- **`shared_dir` 权限**：放在集群 FS 上、两网关都能读写；建议 `0700` 属 lightnfsd 运行
  用户。启动时不可写只告警不拒起（`--check-config` 不写共享目录），但接管会失败。
- **陈旧 `exports.<node>`**：每个节点启动时写 `shared_dir/exports.<自己的 node>` 并与
  其他节点逐一比对导出摘要，不一致则拒绝入集群。**永久下线一个节点后要手工删掉它的
  `exports.<node>`**，否则新节点会与一份过时摘要比对而被拒。
- **GlusterFS `network.ping-timeout ≤ grace/2`**：故障网关的锁随 TCP 断开由砖块清理，
  默认 42s 太长——调到 grace 的一半以内，故障网关的残留锁才能在新网关 grace 内被放掉
  （grace 内客户端的 LOCK(reclaim) 下推失败走 DELAY 重试，见下）。Lustre 的
  `obd_timeout` 同理；CephFS 由接管钩子 `ceph_start_reclaim` 立即驱逐旧会话，无需等超时。

**已知限制**（本方案主体是"主备 + 接管"，非多活）：

- **DRC 与会话槽缓存不跨网关复制**：接管后客户端的 SEQUENCE 在新网关是新会话
  （旧 sessionid → BADSESSION），已确认但客户端未收到应答的非幂等操作会被重放。
  幂等操作无碍；这是 NFSv4.1 exactly-once 语义在网关级故障下的既有边界。
- **委托随故障网关消亡**：故障网关授予的读委托不迁移；客户端用 CLAIM_DELEG_PREV_FH
  被接受为普通 open 状态（名单 + grace 门禁），在 grace 内重新打开，grace 期不再授新委托。
- **每进程一个角色**：failover 模式下一个网关要么整体 active 要么整体 standby；要"按导出
  分角色"、让多台网关同时服务，用 `mode = "active-active"`（§6，设计见 11 册）。
- **接管耗时**：铸新 epoch + 重建协议栈 + 进 grace 通常 < 1s（`lightnfs_cluster_activation_seconds`
  指标覆盖）；客户端感知到的中断还包含 VIP 漂移与 TCP 重连时间，由 keepalived 与客户端
  `timeo`/`retrans` 决定。

## 6. 多网关多活（每导出一个属主网关）

`[cluster] mode = "active-active"`（设计见 11 册）让 N 个网关同时对外
服务：**每个导出（fsid）有且只有一个属主网关**在服务，不同导出可落在不同网关；属主猝死时
它的每个导出各自迁到各自的备选网关（负载自然分散），计划内迁移把一个导出平滑交给指定网关。
客户端由 NFSv4.1 协议本身引导到属主：伪根 `/` 在所有网关一致，跨进一个导出的边界时，非属主
网关回 `NFS4ERR_MOVED` 并在 `fs_locations` / `fs_locations_info` 属性里给出属主网关的地址，
客户端对该导出到属主重新 `EXCHANGE_ID/CREATE_SESSION`。一个导出仍只有一个写者——多活是
导出级并行，不是文件级并行；热点单导出不会因多活变快，建导出时按 fsid 拆分负载。

**配置**（每台网关一份，`nodes` 与后端子表全集群相同，`node` / `node_address` 各自不同）：

```toml
[cluster]
enabled      = true
id           = "3f9c…-uuid"
shared_dir   = "/mnt/cephfs/.lightnfs-cluster"
mode         = "active-active"
node         = "gw1"
node_address = "10.0.0.11:2049"       # 本网关自有地址，fs_locations 里指向本网关的就是它
fence_lease  = "3s"
# role 必须为 auto（默认）；takeover = auto|manual 按导出生效

[[export]]
path = "/export/a"; fsid = 1; backend = "cephfs"
nodes = ["gw1", "gw2", "gw3"]         # 属主优先级：第一个活着的服务，其余按序接管
[[export]]
path = "/export/b"; fsid = 2; backend = "cephfs"
nodes = ["gw2", "gw3", "gw1"]         # b 的属主优先 gw2 → 负载分摊
```

启动时校验：`node_address` 形如 `host:port`、`role` 只能 `auto`、每个导出 `nodes` 非空无重复；
`nodes` 进导出摘要，各网关必须逐字相同（否则拒绝入集群）。多活下 `shared_dir` 里除 09 的
`hmac.key` / `exports.<node>` 外，另有每网关的 `epoch.<node>`（自有 epoch，进程启动 +1）、
`nodes/<node>`（地址登记）、`fence.<node>`（该网关持有的 fsid 集与统一到期时间——**同时是
心跳**，不持任何导出的网关也按周期写空集）、`fs/<fsid>/owner` / `fs/<fsid>/epoch` /
`fs/<fsid>/clients/`（每导出的属主、接管代数、reclaim 名单）。§5 的 NTP、`shared_dir` 权限、
陈旧 `exports.<node>` 三条要点在多活下原样适用。

**地址规划**（这是与 §5 最不同的一处）：

- **入口地址只做伪根的初次接触**：客户端 `mount <入口>:/ /mnt` 落到任一网关，沿伪根向下
  走；入口可以是 DNS 轮询（每个网关一条 A 记录）或一个轻量 VIP——它只需把"第一次
  PUTROOTFH/LOOKUP"送到任一活着的网关，不承载数据面，故障切换也不依赖它漂移。
- **每个网关的 `node_address` 必须能被所有客户端直达**：跨进导出后客户端按 `fs_locations`
  改连属主网关的 `node_address`（Linux 挂成一个子挂载，clientid 按目标网关独立），`[server]
  bind` 要监听这个地址（不要只绑 VIP）。NAT / 防火墙要放行每台网关的 2049。
- **各网关是不同的 server**：`server_owner.major_id = lightnfs-cluster:<id>:<node>`，
  `server_scope = lightnfs-cluster:<id>`（同一管理域，referral/migration 生效的前提）。客户端
  挂了落在不同网关的多个导出，就持有多个 clientid——这是 RFC 8881 的正确行为。

**客户端要求**：

- **Linux NFSv4.1 客户端**（`-o vers=4.1`，4.2 亦可）是目标客户端：服务器在 `EXCHANGE_ID`
  应答里置 `EXCHGID4_FLAG_SUPP_MOVED_REFER | SUPP_MOVED_MIGR`，内核对 referral 自动建子挂载
  （`ls /mnt/export/b` 触发，`/proc/self/mountinfo` 里子挂载目标是属主的 `node_address`），
  对 migration 在 `SEQUENCE` 收到 `SEQ4_STATUS_LEASE_MOVED` 后查 `fs_locations` 跟到新属主并
  reclaim，应用不中断、不重挂载。
- **不支持 referral 的老 v4 客户端**收到 MOVED 会失败：**直接挂到属主网关的 `node_address`**
  （`mount gw2:/export/b`），放弃分流透明性；属主变了要手工换挂。
- **NFSv3 客户端**：v3 没有 `fs_locations`，多活对 v3 只能"每导出挂到其属主网关的地址"，
  由运维/自动化维护导出 → 属主地址的映射（`lightnfs-ctl cluster status` 的 `owner=` /
  `address=` 列）。这是多活对 v3 的明确边界；混挂 v3/v4 的部署，v3 侧要么固定挂属主，
  要么整个部署退回 `mode = "failover"`。
- **无属主的导出**（全部备选都不在）：状态类操作回 `NFS4ERR_DELAY`，客户端按 grace 语义
  重试直到某个备选接管；`lightnfs_cluster_fs_owner{fsid}` 无样本、`cluster status` 里
  `role=unowned owner=none`，据此告警。

**接管与迁移**：

- **自动接管**（`takeover = auto`）：属主的 `fence.<node>` 心跳过期后，该导出 `nodes` 里
  排在前面且仍活着的网关先接；前位活着但 `2 × 3 × fence_lease` 内不接（如 `takeover =
  manual`），后位跳过它接管。不在 `nodes` 里的网关永不自动接管（`--force` 可以）。接管 =
  取围栏、`fs/<fsid>/epoch` +1、只对**该导出**arm grace 并跑该后端的 `takeover()` +
  `takeover_hook`（`LNFS_FSID` 已置），其他导出照常服务；客户端只对该导出 reclaim。
- **手动**：`lightnfs-ctl cluster takeover <fsid> [--force]` / `cluster standby <fsid>`
  与 §5 同义，scope 到一个导出（`standby` 释放后不自动抢回，直到别人持有过或再次
  `takeover`）。
- **计划内迁移**：`lightnfs-ctl cluster migrate <fsid> <node>` **在当前属主上运行**：源对该
  导出进入 Draining（新请求回 MOVED，`fs_locations` 指向目标）、写 `fs/<fsid>/owner` 后释放
  围栏，目标的下一次轮询无视顺位接管；受影响客户端一个租约期内在 `SEQUENCE` 收到
  `LEASE_MOVED`，到目标网关 reclaim。目标必须是活着的同伴（无心跳报错）。
- **滚动升级 / 维护一台网关**：`scripts/cluster_roll.sh evacuate <node> [--to <node>]` 把
  `<node>` 服务的每个导出逐个 `migrate` 到 `--to`，或到该导出 `nodes` 里的下一个活网关，
  并轮询到 `<node>` 不再服务任何导出；然后升级/重启它；`cluster_roll.sh restore <node>` 把
  `nodes[0] == <node>` 的导出迁回。脚本全程经 `lightnfs-ctl cluster status --json` 与
  `cluster migrate` 工作，因此需要每台涉及网关的 ctl 套接字：`--sockets
  gw1=/run/lightnfs/ctl.sock,gw2=…`（远端先把 unix socket 转发到本机）或环境变量
  `LNFS_CTL_SOCKETS`；`evacuate` 只需被撤空网关的套接字，`restore` 需要每个当前属主的。
  每一步失败即停并打印当前属主表。全程无客户端重挂载。
- **迁移窗口**：迁移/接管期间该导出有一个 grace 窗口（默认 = lease，`[protocol] grace` 可
  调短），窗口内新建状态回 GRACE、读放行——只影响该导出，其他导出不受影响。§5 的"DRC /
  会话槽不复制、委托不迁移"两条边界在多活下按导出同样成立。

**后端**：

- **CephFS 最契合**：每导出一个会话 uuid（`[export.cephfs] uuid`，默认 `<cluster id>-<fsid>`），
  接管只 `ceph_start_reclaim` 回收**该导出**的旧会话，同网关正在服务的其他导出不受影响。
  多活优先在 CephFS 上部署。
- **GlusterFS / Lustre 同卷同进退**：libgfapi 的连接、Lustre 的客户端挂载是整卷 / 整挂载级，
  不是按 fsid，单个导出迁走会波及同一网关上同卷的其他导出。因此**同一 Gluster `volume` /
  同一 Lustre `mount` 上的所有导出必须列出完全相同的 `nodes`**（配置校验拒绝不一致的组合），
  要么整卷一个导出、要么整卷的导出一起迁移；`cluster_roll.sh` 逐个 `migrate` 时它们会依次
  到同一目标。§5 的 `network.ping-timeout` / `obd_timeout ≤ grace/2` 要求同样适用。

**观测**：`lightnfs-ctl cluster status`（多活下先一行网关总览：`mode node node_epoch
node_address shared_dir peers peers_alive takeover migrations exports`，再每导出一行
`fsid role nodes owner address fs_epoch fence_age_ms fence_expires_in_ms grace_remaining_s
takeovers fence_lost activation_failures`；`--json` 时 `exports` 为数组）与指标
`lightnfs_cluster_fs_role{fsid,role}`（one-hot：active/activating/draining/remote/unowned）、
`lightnfs_cluster_fs_owner{fsid,node}`、`lightnfs_cluster_fs_epoch{fsid}`、
`lightnfs_cluster_fs_{takeovers,fence_lost,activation_failures}_total{fsid}`、
`lightnfs_cluster_node_epoch`、`lightnfs_cluster_migrations_total`、`lightnfs_v4_moved_total{fsid}`。
告警建议：某 fsid 的 `fs_role{role="active"}` 在集群内之和 ≠ 1、`fs_owner` 长时间无样本、
`fs_fence_lost_total` 增长（脑裂 / 围栏被改写）、`v4_moved_total` 持续增长（客户端没有跟随
`fs_locations`：老客户端或 `node_address` 不可达）。

## 7. 已知限制

- **身份仅 AUTH_SYS**：无 krb5/RPCSEC_GSS（见 §1）。通道加密可用内置 **RPC-over-TLS**
  （`[tls]`，RFC 9289）：STARTTLS 探测 + 同连接 TLS 会话；socket IO 仍全走 io_uring
  （OpenSSL 在 memory BIO 上做密码学，reactor 不阻塞）。回传通道（委托召回 /
  CB_NOTIFY_LOCK）复用同一连接，握手后自动加密。**仍不做写/目录委托、pNFS**（写委托需
  CB_GETATTR + 单写者证据，pNFS 为决策 D8 非目标）；多网关一致性的边界见下文"单机网关"与
  各集群后端条目（CephFS 同时具备原生 change 与原生锁，其余后端各具一半）。
- **不实现 NLM/NSM**：v3 无字节锁（设计 D8）。v4.1 有完整字节锁；**v3 与 v4 同挂一后端时，
  v3 写不受 v4 的 share deny / 字节锁约束**（v3 侧本无锁语义，文档明示的边界）。
- **v4.2 按 op 宣告**：启动时对每个导出探测 `kSparseOps`/`kCopyRange`/`kCloneRange` 并写
  日志（`export <path> v4.2 capabilities: …`）；无能力位的 op 回 NOTSUPP，Linux 客户端自动
  降级（`cp --reflink=auto` 退到 COPY 再退到读写）。READ_PLUS 已支持（稀疏感知，
  每应答一个 DATA/HOLE 段，客户端自续传；无稀疏能力的后端退化为单 DATA 段）。
  不做异步/跨服 COPY、xattr（op 72-75 回 NOTSUPP）、sec_label。CLONE 仅在
  XFS(reflink=1)/Btrfs 导出上可用。
- **READDIR cookie 校验**：cookieverf 取目录 change 属性——分页期间目录被改，客户端
  收到 BAD_COOKIE（v3）/NOT_SAME（v4）并自动从头重新列目录，不再静默漏项/重复。
  高频变更的大目录列举可能因此重启多次（正确性换代价，Linux 客户端自动处理）。
- **读委托 + 回传通道**：会话绑定回传通道（CREATE_SESSION 的
  CONN_BACK_CHAN 或 BIND_CONN_TO_SESSION BACK/BOTH）后，只读 OPEN 可获读委托——
  客户端本地缓存/打开不再逐次询问服务器，GETATTR 风暴显著削减。写打开、SETATTR、
  REMOVE/RENAME、匿名写等冲突操作触发 CB_RECALL 并回 DELAY，客户端 DELEGRETURN
  （或 CLAIM_DELEG_CUR_FH 先转正打开）后重试；一个租约期内不归还则吊销
  （`lightnfs_v4_deleg_revokes_total`）。`[protocol] delegations = false` 可整体关闭。
  锁竞争方也会在持有者解锁时收到 CB_NOTIFY_LOCK 提示重试，替代盲轮询。
  **边界**：与 v3 锁一致，v3 侧的写不触发 v4 委托召回——v3/v4 混挂同一导出时委托
  一致性只覆盖 v4 客户端（文档明示的既有边界）。无写委托/目录委托，无 pNFS。
- **SECINFO 多 flavor**：SECINFO/SECINFO_NO_NAME 恒返回
  `[AUTH_SYS]`（AUTH_SYS-only 服务器的合规简化，永不发 WRONGSEC）。
- **句柄稳定性**：`handles="auto"` 在无 `CAP_DAC_READ_SEARCH` 且文件系统无 STATX_BTIME
  （如 tmpfs）时，句柄不跨重启稳定——生产用 `CAP_DAC_READ_SEARCH` + 内核句柄，或
  `handles="kernel"` 强制（启动即校验，不满足则拒绝启动）。
- **单机网关**：状态在进程内存 + `state_dir` 名单；不做多网关状态共享。`local` 导出
  **不要**同时由多个 lightnfsd 网关导出：即便内核提供原生 change cookie
  （`Cap::kNativeChange`，需 STATX_CHANGE_COOKIE；否则由 ctime 合成、仅本网关内可靠），
  v4 open/deny/字节锁状态也只存在于各网关进程内、互不可见，旁路写同样绕过它们。
  `gluster` 导出把 v4 字节锁下推到卷（posix-locks，`native_locks = true`），多网关之间
  的 LOCK/LOCKT 会互相看见并拒绝；但 change 仍是 ctime 合成（`change_attr_type =
  TIME_METADATA`），open/deny 状态仍是网关本地——跨网关部署时按此边界评估
  （启动日志 `export … backend traits:` 打印每个导出的这些位）。
- **GlusterFS 后端**：libgfapi 是阻塞库，全部调用走 offload 池，`[server]
  offload_threads` 是该后端的吞吐上限旋钮；砖块重连/仲裁丢失期间的传输类错误映射为
  JUKEBOX（v3）/DELAY（v4），客户端重试而非报错（`lightnfs_gluster_jukebox_total`，
  `jukebox = false` 改回 EIO）。身份以 `glfs_setfsuid/gid/groups` 透传，权限由砖块
  判定（`squash = "none"` 时客户端 uid 直达卷；卷侧 `server.root-squash` 会影响以
  网关身份打开的匿名 IO 描述符与锁描述符）。`lightnfs-ctl fdcache` 对 gluster 导出
  显示 glfd/对象缓存/jukebox/锁描述符计数；`clear-poison` 同样适用。
- **CephFS 后端**：唯一同时具备原生 change（`stx_version`，`change_attr_type =
  MONOTONIC_INCR`）与原生字节锁（`ceph_ll_setlk`，MDS 全局仲裁）的后端——多网关同挂一个
  cephfs 导出时，CTO 与 LOCK/LOCKT 都能跨网关看见（v4 open/deny 状态仍是网关本地）。
  身份作为每次调用的 `UserPerm` 交给 libcephfs（`squash = "none"` 时客户端 uid 直达 MDS；
  客户端密钥的 MDS caps 需允许该 subdir 且不限制 uid/gid），ACCESS 则由网关按 mode 位回答
  （POSIX ACL 只在 OPEN/变更判定里生效）。libcephfs 是阻塞库，全部调用走 offload 池；
  MDS failover / OSD 重连期间的传输类错误映射为 JUKEBOX（v3）/DELAY（v4）
  （`lightnfs_cephfs_jukebox_total`），会话被列入黑名单（EBLOCKLISTED）则是硬 EIO 并计数
  `lightnfs_cephfs_blocklisted_total`——出现即需重启网关重连。`lightnfs-ctl fdcache` 对
  cephfs 导出显示 Fh/inode 缓存/jukebox/黑名单/锁句柄计数；`clear-poison` 同样适用；
  `scripts/check_cephapi_abi.sh` 在有 `cephfs/libcephfs.h` 的主机上校验绑定签名与结构布局。
  多网关接管（10 册 D2）：新网关以同一会话 uuid（`[export.cephfs] uuid`，默认
  `<cluster id>-<fsid>`）`ceph_start_reclaim(…, RESET)`，MDS 立即驱逐故障网关的会话并释放其
  caps/锁，客户端在 grace 内的 reclaim 不再被残留锁挡住；standby 会话不带 uuid。MDS 不支持
  （EOPNOTSUPP）或 libcephfs 太旧（缺 `ceph_start_reclaim`）只告警，残留按 MDS 自身会话超时
  （`mds_session_autoclose`，默认 300s）清理——此时 grace 内的 LOCK reclaim 走 DELAY 重试。
- **Lustre 后端**：文件句柄是 FID，经 `<mount>/.lustre/fid` 打开——网关进程**不需要**
  `CAP_DAC_READ_SEARCH` 即可获得跨重启稳定的句柄（本地后端在无该能力时只能走路径兜底）。
  客户端挂载建议 `-o flock`：`native_locks = true` 时 v4 字节锁以 OFD fcntl 锁下推，
  由 MDS 在网关之间与原生客户端之间仲裁（`localflock` 只在本机内有效，此时应关掉
  `native_locks` 以免误判无冲突）。HSM 环境下 `hsm = true`（默认）：读写/截断已释放
  文件不会把 offload/io_uring 工作线程挂在隐式 restore 上，而是提交一次 RESTORE 请求并
  回 JUKEBOX（v3）/DELAY（v4）由客户端重试（`lightnfs_lustre_jukebox_total` /
  `lightnfs_lustre_hsm_restores_total`）；描述符已在 fd 缓存中的文件被释放时，后续 IO
  仍像原生客户端一样阻塞在 restore 上（门禁只在打开时刻）。change 仍是 ctime 合成
  （MDT 发时间戳，跨网关一致，但 `change_attr_type = TIME_METADATA`）。`identity =
  "setfsuid"`（需 root）把鉴权交给 MDS。`lightnfs-ctl fdcache`/`clear-poison` 与本地
  后端相同；无 liblustreapi 依赖，`scripts/check_llapi_abi.sh` 在有 `lustre_user.h` 的
  主机上校验 ioctl 常量。
