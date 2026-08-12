# 8. 配置、可观测性与安全加固

## 8.1 配置

单一 TOML 文件（`/etc/lightnfs/lightnfs.toml`），启动读取；v1 不做热重载（SIGHUP 仅 reopen 日志），导出表变更需重启——文档明示。

```toml
[server]
reactors        = 0          # 0 = 物理核数
offload_threads = 16
port            = 2049
mount_port      = 20048
rpcbind         = true       # 注册系统 rpcbind（纯 v4 环境可关）
builtin_portmap = false
state_dir       = "/var/lib/lightnfs"
max_connections = 4096
max_request_size = "1MiB"

[protocol]
v3       = true
v4       = true              # minorversion 1/2；4.0 恒拒绝
lease    = "90s"
grace    = "90s"             # 亦可 "auto" = lease
drc_ttl  = "120s"
drc_mem  = "64MiB"

[limits]
rtmax = "1MiB"; wtmax = "1MiB"; dtpref = "64KiB"
inflight_per_conn = 64       # v3；v4 由槽数控制
session_slots     = 32

[[export]]                   # 见 06 分册 6.7
path = "/export/data"; backend = "local"; fsid = 1
clients = ["192.168.0.0/24"]; squash = "root"; readonly = false
```

校验规则启动时全量执行（fsid 唯一、路径存在、网段格式），错即拒起——配置错误绝不带病运行。

## 8.2 日志

- 结构化（logfmt/JSON 可选），全异步（per-reactor ring buffer → 落盘线程），热路径日志零分配。
- 级别约定：`error`=数据/协议正确性风险（fsync 失败、白名单外错误映射）；`warn`=可疑客户端行为（BADHANDLE、SEQ_MISORDERED）；`info`=生命周期（挂载、grace、回收）；`debug`=每请求单行摘要。
- **每请求摘要行**（debug，v4 调试第一生产力，nfsv4/11.6）：
  `xid=… peer=… v4 tag="…" ops=[SEQUENCE,PUTFH,OPEN,GETFH,GETATTR] st=OK dur=1.2ms`
- 采样机制：`debug` 关闭时对错误应答自动采样保留最近 N 条完整摘要（环形），`lightnfs-ctl dump-errors` 取出——生产排障不必开全量 debug。

## 8.3 指标（Prometheus 文本口）

| 组 | 指标示例 |
|----|----------|
| rpc | 每程序/过程/操作计数与时延直方图（v4 按 COMPOUND 内 op 展开）、错误码计数 |
| transport | 连接数、在途请求、收发字节、背压触发次数 |
| runtime | reactor 循环延迟、offload 队列深度/等待时延、buffer 池水位 |
| state | 07 分册 7.8 清单 |
| backend | 每后端操作时延直方图、fd 缓存命中率、kJukebox 计数 |
| drc/slots | 命中/重放/in-progress 等待 |

SLI 建议：READ/WRITE p99、GETATTR p99、错误率、grace 时长。

## 8.4 追踪

`OpCtx.trace`（04 分册）贯穿：请求 → core → 后端调用逐段打点；v1 实现为进程内 span 记录 + 慢请求（>阈值）自动落日志；OTLP 导出留接口不实现。

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

- `lightnfs-ctl`：unix socket 管理口——状态表 dump（客户端/会话/打开/锁）、fd 缓存统计、强制回收 client、dump-errors、指标快照。排障闭环不依赖重启。
- `lightnfs-fh`：句柄解码工具（输入 hex 句柄 → fsid/ObjId/HMAC 校验结果），配 wireshark 抓包联调。
