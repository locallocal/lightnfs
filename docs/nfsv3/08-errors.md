# 8. NFSv3 错误码大全

`nfsstat3` 枚举，取值大多沿用 UNIX errno。RFC 1813 明确：**服务器只能返回本表中的错误码**，且每个过程只应返回其规范列出的子集（下文第 8.2 节）；遇到不在表内的本地 errno，映射到语义最近的值，兜底 NFS3ERR_IO 或 NFS3ERR_SERVERFAULT。

## 8.1 错误码定义

| 值 | 名称 | 含义与使用场景 |
|----|------|----------------|
| 0 | NFS3_OK | 成功 |
| 1 | NFS3ERR_PERM | 不是属主/特权不足（chown、mknod 等"必须属主或 root"的场景）。与 ACCES 的区别：PERM = 身份不对，ACCES = 权限位不允许 |
| 2 | NFS3ERR_NOENT | 文件/目录不存在 |
| 5 | NFS3ERR_IO | 底层硬 I/O 错误，兜底错误 |
| 6 | NFS3ERR_NXIO | 设备不存在（罕用） |
| 13 | NFS3ERR_ACCES | 权限检查失败。注意拼写没有第二个 S |
| 17 | NFS3ERR_EXIST | 目标已存在（GUARDED CREATE、MKDIR、LINK、RENAME 目标冲突） |
| 18 | NFS3ERR_XDEV | 跨文件系统操作（RENAME/LINK 跨 fsid） |
| 19 | NFS3ERR_NODEV | 设备不存在（罕用） |
| 20 | NFS3ERR_NOTDIR | 需要目录的参数不是目录 |
| 21 | NFS3ERR_ISDIR | 需要非目录的参数是目录（READ/WRITE 目录、REMOVE 目录） |
| 22 | NFS3ERR_INVAL | 参数语义无效（对非链接 READLINK、目录移入子孙等） |
| 27 | NFS3ERR_FBIG | 文件超过服务器 maxfilesize |
| 28 | NFS3ERR_NOSPC | 空间不足 |
| 30 | NFS3ERR_ROFS | 只读文件系统/只读导出。修改类操作在只读导出上统一回它 |
| 31 | NFS3ERR_MLINK | 硬链接数超 linkmax |
| 63 | NFS3ERR_NAMETOOLONG | 名字超过 name_max |
| 66 | NFS3ERR_NOTEMPTY | RMDIR/RENAME 目标目录非空 |
| 69 | NFS3ERR_DQUOT | 配额超限 |
| 70 | NFS3ERR_STALE | 句柄格式合法但对象已不存在（被删除/文件系统不再导出）。客户端收到后作废该句柄及其缓存，应用层报 ESTALE |
| 71 | NFS3ERR_REMOTE | 路径解析需跳到另一台服务器（多级远程；v3 客户端普遍不支持，基本不用） |
| 10001 | NFS3ERR_BADHANDLE | 句柄格式非法（长度错、内部校验失败）。与 STALE 的区别：BADHANDLE = 根本不是我发的句柄 |
| 10002 | NFS3ERR_NOT_SYNC | SETATTR guard 的 ctime 不匹配 |
| 10003 | NFS3ERR_BAD_COOKIE | READDIR/READDIRPLUS 的 cookie/cookieverf 已失效，客户端应从头重列 |
| 10004 | NFS3ERR_NOTSUPP | 过程不支持（MKNOD、LINK、SYMLINK、READDIRPLUS、EXCLUSIVE CREATE 等可选能力） |
| 10005 | NFS3ERR_TOOSMALL | 请求的 count 预算太小装不下一个响应项（READDIR 连一项都放不进） |
| 10006 | NFS3ERR_SERVERFAULT | 服务器内部错误（不对应任何客户端可理解的 errno 时的兜底） |
| 10007 | NFS3ERR_BADTYPE | MKNOD 的类型不受支持 |
| 10008 | NFS3ERR_JUKEBOX | 操作已启动但需要较长时间（分层存储上线），客户端**稍后用新请求重试**，不应向应用报错。也常被用作服务器过载/背压信号（Linux 客户端映射为重试） |

Linux errno 映射备忘：ESTALE=116（客户端侧），EJUKEBOX 客户端内部处理不上报。服务器侧从本地 errno 生成 nfsstat3 时注意 EACCES→13、EPERM→1、EOPNOTSUPP→10004、ETIMEDOUT/EIO→5。

## 8.2 各过程允许的错误码（RFC 1813 规定）

通用：任何过程都可能返回 IO、SERVERFAULT；带句柄参数的都可能 STALE、BADHANDLE。

| 过程 | 特有/主要错误 |
|------|---------------|
| GETATTR | （仅通用） |
| SETATTR | PERM, ACCES, INVAL, NOSPC, ROFS, DQUOT, NOT_SYNC, ISDIR(设size), FBIG |
| LOOKUP | NOENT, ACCES, NOTDIR, NAMETOOLONG |
| ACCESS | （仅通用） |
| READLINK | INVAL(非链接), ACCES, NOTSUPP |
| READ | NXIO, ACCES, ISDIR, INVAL, JUKEBOX |
| WRITE | ACCES, ISDIR, INVAL, FBIG, NOSPC, ROFS, DQUOT, JUKEBOX |
| CREATE | ACCES, EXIST, NOTDIR, NOSPC, ROFS, NAMETOOLONG, DQUOT, NOTSUPP(EXCLUSIVE) |
| MKDIR | 同 CREATE（无 NOTSUPP） |
| SYMLINK | 同 CREATE + NOTSUPP |
| MKNOD | 同 CREATE + NOTSUPP, BADTYPE, PERM |
| REMOVE | NOENT, ACCES, NOTDIR, NAMETOOLONG, ROFS, ISDIR(Linux 惯例) |
| RMDIR | NOENT, ACCES, NOTDIR, INVAL(./..), ROFS, NAMETOOLONG, NOTEMPTY, NOTSUPP |
| RENAME | NOENT, ACCES, EXIST, XDEV, NOTDIR, ISDIR, INVAL, NOSPC, ROFS, MLINK, NAMETOOLONG, NOTEMPTY, DQUOT, NOTSUPP |
| LINK | ACCES, EXIST, XDEV, NOTDIR, INVAL, NOSPC, ROFS, MLINK, NAMETOOLONG, DQUOT, NOTSUPP |
| READDIR | ACCES, NOTDIR, BAD_COOKIE, TOOSMALL |
| READDIRPLUS | ACCES, NOTDIR, BAD_COOKIE, TOOSMALL, NOTSUPP |
| FSSTAT | （仅通用） |
| FSINFO | （仅通用；BADHANDLE/STALE 仍可能） |
| PATHCONF | （仅通用） |
| COMMIT | （仅通用 + IO） |

## 8.3 错误处理的实现纪律

1. **失败也要带属性**：resfail 分支中的 post_op_attr/wcc_data 尽量填。例如 LOOKUP 返回 NOENT 时带上目录属性，客户端可据此缓存负查找结果（negative dentry）。
2. **不泄露信息**：对无搜索权限的目录，LOOKUP 应返回 ACCES 而不是区分 NOENT/EXIST。
3. **STALE vs BADHANDLE 分清楚**：句柄能解析但对象没了 → STALE；解析都过不了 → BADHANDLE。客户端对两者的恢复行为不同。
4. **JUKEBOX 不要滥用**：客户端会静默重试，滥用会把故障变成无限挂起；只用于确定"稍后会好"的场景。
