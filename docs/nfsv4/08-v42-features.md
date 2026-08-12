# 8. NFSv4.2 新特性（RFC 7862）

4.2 不改动 4.1 的骨架（会话/状态模型原样），只增操作与属性，且**全部可选**——服务器逐个特性宣告支持与否（不支持的操作回 NFS4ERR_NOTSUPP，客户端逐个探测降级）。这使 4.2 成为"甜点拼盘"：按需捡有价值的做。

## 8.1 服务器端拷贝（COPY / COPY_NOTIFY / OFFLOAD_*）

`cp` 大文件时数据从服务器读到客户端再写回服务器，两倍网络流量纯属浪费。4.2 让拷贝在服务器端完成：

```
同服拷贝：
  { SAVEFH(src); PUTFH(dst); COPY(src_stateid, dst_stateid,
     src_offset, dst_offset, count, ca_consecutive, ca_synchronous) }
  → 同步完成：返回已拷字节数 + write verifier
  → 异步执行：返回 copy stateid，完成时服务器发 CB_OFFLOAD 通知；
     客户端可 OFFLOAD_STATUS 查询 / OFFLOAD_CANCEL 取消

跨服拷贝（inter-server）：
  客户端→源服务器: COPY_NOTIFY(告知目的服务器身份) → 授权凭据
  客户端→目的服务器: COPY(带源信息)；目的服务器扮演 NFS 客户端去源端拉数据
```

- Linux 客户端 4.2 挂载下 `copy_file_range(2)` 直达 COPY；同服拷贝 knfsd 已支持，跨服（inter-server copy）Linux 5.7+ 支持但默认关。
- 服务器实现：同服 COPY 映射到 `copy_file_range`/reflink/读写循环皆可；异步模式需要 offload 状态跟踪 + 回调，**先只做同步模式**（ca_synchronous 强制 TRUE）是合理简化。

## 8.2 CLONE（reflink）

```
CLONE(src_stateid, dst_stateid, src_offset, dst_offset, count)   /* count=0 → 整文件 */
```

- 要求底层支持写时复制共享 extent（XFS/Btrfs 的 `FICLONERANGE`），**瞬间完成、原子**，与 COPY 的"搬运数据"本质不同。
- 属性 `clone_blksize` 声明对齐粒度。不支持 → NOTSUPP，客户端 fallback 到 COPY 再到读写循环。

## 8.3 稀疏文件四件套

| 操作 | 对应本地接口 | 语义 |
|------|-------------|------|
| SEEK(stateid, offset, what{SEEK_HOLE/SEEK_DATA}) | lseek(2) 同名 | 找下一个洞/数据区，返回 (eof, offset) |
| ALLOCATE(stateid, offset, length) | fallocate(0) | 预分配保证后续写不 ENOSPC |
| DEALLOCATE(stateid, offset, length) | fallocate(PUNCH_HOLE) | 打洞，读回为零 |
| READ_PLUS(stateid, offset, count) | — | 读结果为**内容数组**：DATA 段 + HOLE 段（洞不传零字节） |

- READ_PLUS 是 READ 的超集，节省稀疏文件/大洞的传输量；服务器把洞报告为 HOLE 段是可选的（全报 DATA 也合规）。Linux 服务器一度因性能回退默认只在明确稀疏时用。
- 属性 `space_freed` 配合 DEALLOCATE 汇报释放量。
- 实现成本低（直接映射 lseek/fallocate），**是 4.2 特性里性价比最高的一组**。

## 8.4 IO_ADVISE

posix_fadvise 的网络化：WILLNEED/DONTNEED/SEQUENTIAL/RANDOM/NOREUSE 等 hint，服务器"尽力参考"。无硬语义，实现可以只回"收到"。

## 8.5 WRITE_SAME 与 ADB

按"应用数据块（ADB：块大小 + 模式串 + 编号规则）"批量初始化文件内容（如数据库预格式化）。极少被客户端使用（Linux 客户端未实现），**直接 NOTSUPP**。

## 8.6 安全标签（sec_label，属性 80）

把 SELinux/SMACK 等 MAC 标签作为文件属性存取，配套 CB_NOTIFY 的标签变更通知，使"带标签的 NFS 根文件系统"（容器/无盘工作站）可行。需要客户端、服务器、LSM 策略三方配合（Linux `security_label` 导出选项）。通用文件服务场景可忽略。

## 8.7 4.2 相关的周边 RFC（同期生态）

- **RFC 8276 xattr**：GETXATTR(72)/SETXATTR(73)/LISTXATTRS(74)/REMOVEXATTR(75) + xattr_support 属性——真正的 user.* 扩展属性支持（Linux 5.9+ 双端支持）。严格说是独立扩展，不属于 RFC 7862，但实践中与 4.2 一起出现。
- RFC 8275 mode_umask 属性：解决 CREATE 时 umask 与默认 ACL 的交互。
- RFC 8587 / RFC 8178：小版本演进与勘误机制。

## 8.8 lightnfs 取舍建议

| 特性 | 建议 | 理由 |
|------|------|------|
| SEEK/ALLOCATE/DEALLOCATE | **做** | 映射系统调用即可，cp/rsync 稀疏文件直接受益 |
| 同步同服 COPY | **做** | copy_file_range 一行映射，cp 大文件流量减半 |
| CLONE | 底层是 XFS/Btrfs 就做 | FICLONERANGE 直通 |
| READ_PLUS | 可后做 | 先当 READ 实现（全 DATA 段）也合规 |
| 异步/跨服 COPY、WRITE_SAME、sec_label、IO_ADVISE、xattr | 不做/后做 | 状态跟踪复杂或场景小众 |

注意：宣告 minorversion=2 不等于要支持所有 4.2 操作——逐操作 NOTSUPP 是协议明文允许的（RFC 7862 §4.2 的 operation table 标注 OPT）。因此"实现 4.1 骨架 + 挑三个便宜的 4.2 操作"就可以对外提供 vers=4.2 挂载。
