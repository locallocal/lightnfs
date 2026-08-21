# M2+M3：NFSv3 读写生产化（阶段 2）

在 M1 只读基础上补齐全部 21 个 NFSv3 过程：SETATTR/WRITE/COMMIT、CREATE（三模式）/
MKDIR/SYMLINK/MKNOD/REMOVE/RMDIR/RENAME/LINK，加上 DRC、boot-epoch 写校验子、fd 缓存
完备、可观测性与管理工具最小版。

## 写路径语义

- **稳定级**：WRITE 三档直通后端——Unstable=pwrite、DataSync=pwrite+fdatasync、
  FileSync=pwrite+fsync；COMMIT=fdatasync。**fsync/fdatasync 一旦 EIO，该文件被标记，
  后续 COMMIT 恒返回 EIO**（writeback 错误绝不吞掉，设计 06 §6.2）。
- **写校验子**：`state_dir/boot_epoch` 每次启动 +1（write+fsync+rename 持久化），其值
  即 WRITE/COMMIT verifier。kill -9 重启后 verifier 变化，客户端按协议重发未提交数据。
- **CREATE EXCLUSIVE**：verifier 持久化在 atime（低 32 位）/mtime（高 32 位）；EEXIST
  时读回比对——重传返回成功，真冲突返回 EEXIST；跨进程重启同样成立（契约单测覆盖）。
- **CREATE UNCHECKED** 对已存在文件成功，仅应用 size 属性（截断）；GUARDED 严格 EEXIST。
- **WCC**：全部 21 个过程含失败分支都带 wcc_data / post_op_attr（core mutate 模板：
  exclusive 锁 → before 采样 → 后端 op → after 采样；失败路径同样采样 after）。
- **RENAME/LINK** 双对象按 ObjId 排序取锁；跨导出 RENAME/LINK 拦截为 XDEV。
- SETATTR 支持 guard ctime（不匹配 → NOT_SYNC）。

## DRC（重复请求缓存）

9 个非幂等过程（SETATTR/CREATE/MKDIR/SYMLINK/MKNOD/REMOVE/RMDIR/RENAME/LINK）进入
DRC；key = {peer, xid, prog, vers, proc, args 前 256B 校验和}。三态：miss（执行并缓存
应答字节）、in-progress（重传挂 AsyncCondVar 等原应答，绝不并发重执行）、done（原字节
重放）。TTL（默认 120s）+ 内存上限（默认 64MiB）淘汰，`[protocol] drc_ttl/drc_mem` 可配。

## fd 缓存与身份执行

- FdCache：每对象一个缓存 fd，读取用 O_RDONLY，写触发就地升级 O_RDWR（旧引用经
  shared_ptr 自然退役）；容量水位驱逐；统计经 `lightnfs-ctl fdcache`。
- 身份模式（`[export.local] identity`）：`check`（默认，权限位自查 + v3 属主放宽）、
  `strict`（faccessat2(AT_EACCESS) 切 fsuid 复核，覆盖 ACL）、`setfsuid`（offload 线程
  切 fsuid/fsgid，内核权威判定；需 root，附组不切换为已知限制）。

## 可观测性与工具

- 异步结构化日志：调用点栈上定长格式化（零分配）→ 环形槽 → 落盘线程；满环丢弃计数。
  `debug` 级输出每请求单行摘要（proc/xid/peer/status）。
- 错误应答环形采样（最近 64 条），`lightnfs-ctl dump-errors` 取出。
- Prometheus 文本指标：rpc（每过程 calls/errors/duration）、transport（连接/拒绝/背压）、
  io 字节、DRC、fd 缓存。经 `lightnfs-ctl metrics` 或 `[server] metrics_port` HTTP 口。
- `lightnfs-ctl [--socket PATH] ping|metrics|dump-errors|drc|fdcache`（unix socket，
  默认 `<state_dir>/ctl.sock`，环境变量 `LIGHTNFS_CTL` 可覆盖）。
- `lightnfs-fh [--key hmac.key] <hex>`：句柄离线解码 + HMAC 校验（配抓包联调）。

## 验收（开发计划 §4.6）

**回环半（无 root）**：`scripts/accept_m2_local.sh [ASAN_SECS]` 一键——两配置单测、
`wtest`（三稳定级写/commit/setattr/exclusive 重放/命名空间操作/线上 DRC 重传逐字节
校验）、kill -9 崩溃恢复（verifier 变化 + 数据重发收敛）、10k 连接风暴 + 超深流水线
背压、管理工具冒烟、ASAN 长稳（服务器平滑退出 LeakSanitizer 判定）。

**真实 mount 半（root VM，CI `m2-acceptance` 作业）**：`scripts/accept_m2_vm.sh [FSX_OPS]`
——rw 挂载后 cthon04 basic/general/special 全量（`fetch_cthon.sh`）、fsx（`fetch_fsx.sh`
独立构建 xfstests fsx；每 PR 5 万 ops，过夜跑传大 FSX_OPS 如 1000 万）、kill -9 后同一
挂载无感恢复（免 remount 读写检查）。
