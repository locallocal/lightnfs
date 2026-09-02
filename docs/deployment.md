# 部署与运维（v1 发布）

lightnfs 是一个用户态 NFS 网关（NFSv3 + NFSv4.1/4.2，读写），面向"受信网络内导出本地目录树"
的场景。本文是发布运维的落地指南：信任边界、最小特权部署、配置要点、可观测性与已知限制。

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
- 仅两个 capability：`CAP_DAC_READ_SEARCH`（`open_by_handle_at` 稳定句柄，设计 06）与
  `CAP_NET_BIND_SERVICE`（绑定 2049/20048），其余 `CapabilityBoundingSet` 清空、
  `NoNewPrivileges`；
- 文件系统沙箱：`ProtectSystem=strict` + 仅 `state_dir` 与显式列出的导出树可写；
- seccomp 白名单：`@system-service` 去掉高危集，再显式加入 io_uring 与句柄系统调用
  （允许集由 `scripts/gen_seccomp_allowlist.sh` 从真实 v3+v4.1 读写+锁+v4.2 稀疏/拷贝负载
  的 strace 生成，运行时若变更需复核；v4.2 用到的 `lseek`/`fallocate`/`copy_file_range`/
  `ioctl(FICLONERANGE)` 与探测用 `openat(O_TMPFILE)` 均在 `@system-service` 内，无需额外放行）。

打包安装（plan doc 10 §4.5）：`packaging/make_tarball.sh` 产出
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
| 后端 | `[[export]] backend` + `[export.local]` / `[export.gluster]` | `local`（本机目录树）或 `gluster`（libgfapi 卷：`volume`/`servers`/`subdir`；运行时加载 `libgfapi.so.0`，缺库启动失败并写明；`path` 只是挂载名） |
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
  不可逆）、`grace-end`（提前结束 grace）。所有命令加 `--json` 输出机器可读 JSON。
  另有本地子命令 `lightnfs-ctl bench <echo|nullrpc|fullpath>`——三层基准（02 分册
  §2.8），自起进程内栈压测，不经 ctl 套接字、不涉运行中的服务。
- **热重载**（SIGHUP 或 `lightnfs-ctl reload`，plan doc 10 §4.1 第一步）：重新解析
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
- **慢请求日志**：超过 `[server] slow_request_ms`（默认 1000，0 关闭）的请求落一条
  warn 日志；v4 附 COMPOUND 内逐 op 耗时分解（`ops=[PUTFH=12us,READ=890000us]`），
  现网定位的第一工具。`dump-errors` 采样环大小由 `[server] error_ring`（默认 64）控制。
- **重启恢复**：进程重启后 boot_epoch +1，读 `state_dir/clients/` 名单进入 grace
  （时长 = lease），仅名单内客户端可 reclaim；名单内全部 RECLAIM_COMPLETE 则提前结束。
  普通操作在 grace 内收 GRACE 重试。**不要清空 state_dir**，否则客户端无法 reclaim、
  可能丢未提交写。

## 5. 已知限制（v1）

- **身份仅 AUTH_SYS**：无 krb5/RPCSEC_GSS（见 §1）。通道加密可用内置 **RPC-over-TLS**
  （`[tls]`，RFC 9289）：STARTTLS 探测 + 同连接 TLS 会话；socket IO 仍全走 io_uring
  （OpenSSL 在 memory BIO 上做密码学，reactor 不阻塞）。回传通道（委托召回 /
  CB_NOTIFY_LOCK）复用同一连接，握手后自动加密。**仍不做写/目录委托、pNFS、多网关一致性**
  （前提未变：写委托需 CB_GETATTR + 单写者证据，pNFS 为决策 D8 非目标，多网关需下沉原生锁的
  集群后端）。
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
- **读委托 + 回传通道**（plan doc 10 §5.2）：会话绑定回传通道（CREATE_SESSION 的
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
