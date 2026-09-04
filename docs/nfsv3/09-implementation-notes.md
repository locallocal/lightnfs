# 9. 实现要点与设计建议（面向 lightnfs）

综合前面各分册，本文归纳把 NFSv3 服务器做**对**、做**稳**、做**快**的工程要点，按优先级排列。

## 9.1 文件句柄设计（最重要的单项决策）

句柄必须：唯一、持久（跨重启有效）、对象删除后可检测失效、≤64 字节、**不可伪造/不可遍历**。

**方案 A：inode 直编码（导出真实文件系统时的经典方案）**

```
fh = { fsid(4B), ino(8B), generation(4B) }    共 16 字节
```

- generation 来自文件系统（Linux: `ioctl(FS_IOC_GETVERSION)` 或 `name_to_handle_at`），防 inode 复用后旧句柄命中新文件。
- 用户态实现的难题：**从 (ino, gen) 反查打开文件**。Linux 用户态可用 `open_by_handle_at(2)`（需要 CAP_DAC_READ_SEARCH），这是用户态 NFS 服务器最干净的路径；否则要维护 ino→path 映射表并处理 rename 导致的失配。
- 安全弱点：句柄可预测（ino 可枚举）。补强：句柄内加入 HMAC 截断（如 8 字节，密钥服务器持有），验证不过返回 BADHANDLE。

**方案 B：句柄=随机 ID + 持久映射表**

- 分配随机 64/128 位 ID，映射表（ID → 对象）落盘。天然不可猜测；但表要持久化、要与文件系统状态保持同步（外部改名/删除会脱节），适合"服务器独占管理存储"的形态（如导出自建对象层/打包文件），不适合导出一棵别人也在动的目录树。

lightnfs 若定位为"导出本地目录"，推荐 **方案 A + open_by_handle_at + HMAC**；若无特权可退化为 ino→path 缓存 + 定期校验（接受外部 rename 下的 STALE 误报）。

**持久性红线**：句柄跨重启必须稳定。任何"重启后重新分配 ID"的设计都会导致客户端全部句柄 STALE、已挂载的客户端集体报错，等同数据不可用。

## 9.2 重复请求缓存（DRC）

无状态 + 重传 ⇒ 非幂等操作（REMOVE/RENAME/CREATE/MKDIR/...）重放会产生虚假错误（NOENT/EXIST）。

- 键：(xid, 源 IP, 源端口, 过程号)，可加请求参数校验和防 xid 撞车。
- 值：完整编码后的应答；LRU/定时淘汰（数秒到数十秒即可覆盖重传窗口）。
- 命中时**直接重放缓存应答**，不重新执行。
- 只需覆盖非幂等过程（READ/GETATTR 等可绕过 DRC 省内存）。
- TCP 也需要：断线重连后的重发同样是重复请求。
- 进阶：正在执行中的请求（in-progress）收到重复时应丢弃或挂起，而非并发再执行一次。

## 9.3 并发模型与原子性

- **每个文件/目录一个串行化点**：WCC 的 before/after 原子采样、RENAME 原子性、目录修改 vs READDIR 遍历，都需要 per-object 锁（按句柄 hash 分桶即可）。
- RENAME 涉及两个目录：按固定顺序（如句柄字典序）取锁防死锁。
- 事件循环 + 工作线程池是常见结构：I/O 线程做帧解析/编解码，慢操作（磁盘）丢给 worker；注意同一文件的请求保序或加锁。
- TCP 流水线：单连接上并发处理多个请求、乱序应答（xid 定位）是性能关键；但**同一文件的重叠 WRITE 保持到达顺序**更稳妥。

## 9.4 写路径策略

按实现阶段递进：

1. **v0（最简正确）**：所有 WRITE 无视 stable 参数直接 `pwrite+fdatasync`，返回 committed=FILE_SYNC；COMMIT 直接成功。verf 恒定。正确但写吞吐差。
2. **v1（标准做法）**：UNSTABLE 写只 `pwrite`（进 OS 页缓存），COMMIT 时 `fdatasync`；verf = 服务器启动时间。DATA_SYNC/FILE_SYNC 请求按档执行。
3. 优化：聚合相邻 UNSTABLE 写、COMMIT 用 `sync_file_range` 限定区间、O_DIRECT 大块直写等，按 profile 再说。

**红线**：返回过 FILE_SYNC/COMMIT 成功的数据绝不能丢；verf 必须在"可能丢过数据的重启"后改变。

## 9.5 消息与内存

- 单条消息上限 = max(wtmax + 请求头余量, READDIR maxcount 等)，解析前先校验记录标记长度，超限直接断连（防内存放大 DoS）。
- 变长字段（opaque/string/数组个数）解码时逐个上限校验，**先查后分配**。
- READ 应答的零拷贝：应答 = [RPC头+属性+count+eof+数据长度][文件数据][填充]，可以 `preadv` 直读进应答缓冲，或 writev 拼接，避免中间拷贝。
- 大 IO 缓冲复用池，避免每请求 malloc 1MB。

## 9.6 安全清单

1. **NFS 层导出校验**：每个请求解析句柄后核对 fsid 是否在导出表、源 IP 是否允许——不要只依赖 MOUNT 时的检查（句柄可被离线构造）。
2. **句柄防伪**：HMAC（见 9.1）。
3. **路径逃逸**：LOOKUP `..` 在导出根必须拦住；服务器内部任何 name 拼接前校验无 `/` 与 NUL。
4. **squash**：root_squash（uid0→匿名）默认开启；all_squash 可选。squash 发生在**授权与落盘属主**两处。
5. **AUTH_SYS 即无安全**：文档明示仅适用于受信网络；RPCSEC_GSS 留扩展点（未实现）。通道加密已由 **RPC-over-TLS**（RFC 9289，`[tls] mode = off|optional|required`）提供，在 RPC 层之下对 v3/MOUNT 同样生效；身份仍是 AUTH_SYS 声明。
6. 资源限制：每连接未完成请求数上限、连接数上限、READDIR 预算强制执行。

## 9.7 与真实客户端的兼容性备忘（踩坑清单）

- **Linux 客户端**对 READDIR cookieverf 变化敏感：verf 要么全 0，要么像 lightnfs 这样取目录 change 属性并在 cookie≠0 时校验（变则回 BAD_COOKIE 让客户端从头重列——客户端自动处理）；cookie 必须稳定，别用会因删除位移的数组下标。
- READDIRPLUS 一定要实现，否则目录密集负载 RPC 数爆炸；但注意 Linux 对大目录会自动切回 READDIR（nordirplus 挂载项存在）。
- EXCLUSIVE CREATE 要支持（O_EXCL 依赖），记得 verifier 存进 atime/mtime 且 SETATTR 能洗掉。
- post_op 属性尽量都带上：不带会引发客户端补发 GETATTR，性能减半。
- fileid 必须稳定且 fs 内唯一：Linux 客户端用它当 inode 号，重复的 fileid 会触发 readdir 循环检测报错（"directory contains a readdir loop"）。
- WRITE 返回的 count 短写合法，但频繁短写会让客户端重发风暴，尽量全量完成。
- 时间戳给纳秒精度，否则 make/rsync 类工具行为异常（见 [07-caching-consistency.md](07-caching-consistency.md)）。
- `rpcinfo -p server`、`showmount -e server`、`mount -o vers=3,tcp`、挂载后 `nfsstat -c`、抓包 `wireshark`（NFS 解析器极好）是联调基本功。

## 9.8 测试工具与验收

- **功能**：connectathon test suite（cthon04，行业标准：basic/general/special/lock 四组）；pynfs 主要面向 v4，v3 部分有限；fsstress/fsx（数据一致性殊死测试，fsx 跑一夜不挂再谈发布）。
- **互操作**：Linux 内核客户端（多内核版本）、macOS 客户端各挂载跑一遍。
- **崩溃语义**：kill -9 服务器于 UNSTABLE 写后、COMMIT 前，验证客户端重发且文件最终内容正确；重启后旧句柄仍有效。
- **模糊测试**：对 XDR 解码器做 fuzz（AFL/libFuzzer 直喂消息字节），解码器是最大攻击面。
- **性能基线**：fio 顺序/随机大小块读写、mdtest/smallfile 元数据操作；对比同机 Linux knfsd 得出差距数量级。

## 9.9 建议的实现里程碑（已全部交付，现行状态见 design/09-roadmap.md）

M1–M3 全部实现；M4 的可选项已决定：MKNOD **已实现**、NFSv4（4.1/4.2）**已实现**、
NLM 锁**不做**（`-o nolock` 定位，见 06 分册）、RPCSEC_GSS **不做**（通道加密改由
RPC-over-TLS 提供）。

1. **M1 只读服务器**：RPC/XDR 框架 + TCP 记录标记 + portmap 注册 + MOUNT(MNT/EXPORT) + NULL/GETATTR/LOOKUP/ACCESS/READ/READDIR/READDIRPLUS/FSSTAT/FSINFO/PATHCONF/READLINK。Linux 能挂载、能 `ls -lR`、能 `cat`，即里程碑达成。
2. **M2 可写**：WRITE(先全同步)/CREATE(三模式)/SETATTR/REMOVE/MKDIR/RMDIR/RENAME/LINK/SYMLINK/COMMIT + DRC + WCC。跑通 cthon basic/general。
3. **M3 生产化**：UNSTABLE 写路径 + verf、句柄 HMAC、NFS 层导出校验、资源限制、fsx 过夜。
4. **M4 可选**：MKNOD、NLM 锁（或明确 nolock 定位）、RPCSEC_GSS、NFSv4。
