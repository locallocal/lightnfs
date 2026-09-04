# 4. NFSv3 的 22 个过程详解

程序号 100003，版本 3，过程 0–21。所有结构沿用 [03-data-types.md](03-data-types.md) 的定义；错误码语义见 [08-errors.md](08-errors.md)。

约定：每个过程的结果均以 `nfsstat3 status` 开头，下文的 "resok / resfail" 指成功/失败分支。**失败分支携带的 post_op_attr / wcc_data 同样应该尽量填充**。

| # | 过程 | 语义 | 幂等 | 修改性 |
|---|------|------|------|--------|
| 0 | NULL | 探活/ping | 是 | 否 |
| 1 | GETATTR | 取属性 | 是 | 否 |
| 2 | SETATTR | 设属性/truncate | 视情况 | 是 |
| 3 | LOOKUP | 名字→句柄 | 是 | 否 |
| 4 | ACCESS | 权限查询 | 是 | 否 |
| 5 | READLINK | 读符号链接 | 是 | 否 |
| 6 | READ | 读数据 | 是 | 否 |
| 7 | WRITE | 写数据 | 是* | 是 |
| 8 | CREATE | 建常规文件 | 否 | 是 |
| 9 | MKDIR | 建目录 | 否 | 是 |
| 10 | SYMLINK | 建符号链接 | 否 | 是 |
| 11 | MKNOD | 建设备/socket/fifo | 否 | 是 |
| 12 | REMOVE | 删文件 | 否 | 是 |
| 13 | RMDIR | 删目录 | 否 | 是 |
| 14 | RENAME | 改名/移动 | 否 | 是 |
| 15 | LINK | 建硬链接 | 否 | 是 |
| 16 | READDIR | 列目录（仅名字） | 是 | 否 |
| 17 | READDIRPLUS | 列目录（含属性/句柄） | 是 | 否 |
| 18 | FSSTAT | 文件系统动态信息（df） | 是 | 否 |
| 19 | FSINFO | 文件系统静态能力 | 是 | 否 |
| 20 | PATHCONF | POSIX pathconf | 是 | 否 |
| 21 | COMMIT | 提交异步写 | 是 | 落盘 |

\* 同一 (offset,count,data) 的 WRITE 重放结果相同，可视为幂等，但重放会重复推进 mtime。

---

## 0. NULL — 空过程

```
参数: void        结果: void
```

不做任何事。用于探活、测量 RTT、rpcinfo。**不需要认证**，不得因凭证问题拒绝。

## 1. GETATTR — 获取属性

```
参数: nfs_fh3 object
resok: fattr3 obj_attributes        resfail: void
```

- 唯一一个"失败不带属性"的过程（失败即无属性可言）。
- NFS 流量中占比极高（客户端属性缓存过期就发它），必须实现得快。
- 不做权限检查（惯例：能拿到句柄就能 stat）。

## 2. SETATTR — 设置属性

```
参数: nfs_fh3 object; sattr3 new_attributes;
      union sattrguard3 switch (bool check) {
          case TRUE: nfstime3 obj_ctime;   /* 守卫 */
          case FALSE: void; }
resok/resfail: wcc_data obj_wcc
```

- `guard.check=TRUE` 时：若对象当前 ctime ≠ obj_ctime，返回 **NFS3ERR_NOT_SYNC**，不做修改。这是 v3 提供的简易"乐观并发控制"，防止两个客户端的 setattr 互相覆盖。
- 设置 `size` 即 truncate（缩短丢弃数据）或 extend（补零）；对目录设 size 应返回 NFS3ERR_ISDIR，对非常规文件 NFS3ERR_INVAL。
- 权限规则遵循 POSIX：改 mode/uid/gid 需属主或 root；`SET_TO_SERVER_TIME` 需属主或写权限；`SET_TO_CLIENT_TIME` 需属主（或 root）。
- 非 root 用户 chown 通常应拒绝（NFS3ERR_PERM）；chgrp 仅限自己所属组。
- 实现注意：truncate 可能因强制锁（mandatory lock）被拒 → 可返回 NFS3ERR_ACCES。

## 3. LOOKUP — 目录内查名字

```
参数: diropargs3 what { dir, name }
resok:  nfs_fh3 object; post_op_attr obj_attributes; post_op_attr dir_attributes;
resfail: post_op_attr dir_attributes;
```

- 单分量查找；`.` 返回目录自身，`..` 返回父目录（挂载点根的 `..` 惯例返回自身或 NFS3ERR_ACCES，视导出策略）。
- **不跨文件系统**：若 name 是挂载点，传统服务器返回其下层目录本身的句柄（fsid 不同暴露给客户端时行为由实现决定；Linux 服务器有 crossmnt 选项）。lightnfs 每个 `[[export]]` 自带 fsid 与后端，导出之间不互相嵌套、不跨挂载点（v4 由伪根拼接），故 LOOKUP 内不做 crossmnt。
- dir 不是目录 → NFS3ERR_NOTDIR。name 不存在 → NFS3ERR_NOENT。
- 权限：需要 dir 的执行（搜索）权限。
- 这是路径解析的原语，性能关键；返回两份属性是为了同时刷新子对象和目录的缓存。

## 4. ACCESS — 权限查询

```
参数: nfs_fh3 object; uint32 access;    /* 想查询的权限位掩码 */
resok:  post_op_attr obj_attributes; uint32 access;   /* 实际允许的子集 */
resfail: post_op_attr obj_attributes;
```

权限位：

```c
#define ACCESS3_READ    0x0001   /* 读文件数据 / 读目录 */
#define ACCESS3_LOOKUP  0x0002   /* 目录内查找（仅目录有意义） */
#define ACCESS3_MODIFY  0x0004   /* 修改既有内容/目录项 */
#define ACCESS3_EXTEND  0x0008   /* 追加/新增 */
#define ACCESS3_DELETE  0x0010   /* 删除目录内条目（仅目录有意义） */
#define ACCESS3_EXECUTE 0x0020   /* 执行文件（对目录不用它，用 LOOKUP） */
```

- 服务器按**请求凭证在服务器视角下的最终身份**（含 root squash、ACL）裁决，返回允许位的子集；只需回答被询问的位，多回答也无害。
- v3 引入本过程就是为了解决"客户端本地猜权限猜不对"的问题（uid 映射、squash、ACL 客户端都不知道）。
- 注意：ACCESS 结果客户端会缓存；服务器不应依赖"客户端每次操作前都问过"。真正的检查仍要在各操作内做。

## 5. READLINK — 读符号链接内容

```
参数: nfs_fh3 symlink
resok:  post_op_attr symlink_attributes; nfspath3 data;
resfail: post_op_attr symlink_attributes;
```

- 仅对 NF3LNK 有效，其他类型返回 NFS3ERR_INVAL。
- 链接内容是**不透明字节串**，服务器不解析、不解引用；客户端拿回去自己接着做路径解析（可能指向客户端本地路径）。

## 6. READ — 读文件数据

```
参数: nfs_fh3 file; offset3 offset; count3 count;
resok:  post_op_attr file_attributes; count3 count; bool eof; opaque data<>;
resfail: post_op_attr file_attributes;
```

- 返回的 `count` = data 长度，可以**短读**（小于请求量），客户端必须继续发后续 READ；`eof=TRUE` 表示本次已读到文件末尾。
- offset ≥ 文件大小：返回 count=0, eof=TRUE（不是错误）。
- count 超过服务器 rtmax：服务器可以只返回 rtmax 字节，不应报错。
- 对目录 READ → NFS3ERR_ISDIR；对符号链接/设备 → NFS3ERR_INVAL。
- 权限：读权限；**惯例放宽**——属主即便没有读权限位，某些服务器也允许（因为本地语义里 open 时有权限即可，而 NFS 没有 open；Linux 服务器对属主放宽以支持先 open 后 chmod 的程序）。lightnfs 建议跟随 Linux 惯例：检查失败但请求者是属主时放行读写。
- 零拷贝机会：data 是消息尾部字段，可以 readv/sendfile 拼接（见 [09-implementation-notes.md](09-implementation-notes.md)）。

## 7. WRITE — 写文件数据

```
enum stable_how { UNSTABLE = 0, DATA_SYNC = 1, FILE_SYNC = 2 };

参数: nfs_fh3 file; offset3 offset; count3 count; stable_how stable; opaque data<>;
resok:  wcc_data file_wcc; count3 count; stable_how committed; writeverf3 verf;
resfail: wcc_data file_wcc;
```

v3 最重要的改进。三档稳定性：

| stable 请求 | 服务器义务 | 典型实现 |
|-------------|-----------|----------|
| UNSTABLE | 可以只写入内存缓存，日后落盘 | write() 进页缓存即可返回 |
| DATA_SYNC | 返回前数据必须落盘（元数据可迟） | 相当于 O_DSYNC |
| FILE_SYNC | 返回前数据 + 元数据落盘 | 相当于 O_SYNC / write+fsync |

- 服务器返回的 `committed` 可以**强于**请求（请求 UNSTABLE，回 FILE_SYNC 是合法且常见的——同步实现最简单），但不得弱于请求。
- `verf`（write verifier）：8 字节，服务器每次**重启/丢失未落盘数据**时必须改变（典型取启动时间）。客户端缓存 UNSTABLE 写过的数据直到 COMMIT 成功；若 COMMIT/后续 WRITE 返回的 verf 变了，说明服务器中途重启，客户端重发全部未提交数据。
- 短写合法（count < 请求 count），客户端会重发剩余部分。count=0 的写：直接返回成功即可。
- offset 超过文件末尾：合法，中间是空洞（稀疏文件）。
- WRITE 不改文件大小之外的语义细节：写入会推进 mtime/ctime；size 只增不减（增长到 offset+count 若超过原 size）。
- 权限：写权限（同 READ 有属主放宽惯例）。ROFS、FBIG、NOSPC、DQUOT 是常见错误。

## 8. CREATE — 创建常规文件

```
enum createmode3 { UNCHECKED = 0, GUARDED = 1, EXCLUSIVE = 2 };

参数: diropargs3 where { dir, name };
      union createhow3 switch (createmode3 mode) {
          case UNCHECKED:
          case GUARDED:   sattr3 obj_attributes;
          case EXCLUSIVE: createverf3 verf; }
resok:  post_op_fh3 obj; post_op_attr obj_attributes; wcc_data dir_wcc;
resfail: wcc_data dir_wcc;
```

三种模式：

- **UNCHECKED**：存在也成功（等价 open(O_CREAT)），已存在时不应用 sattr（除了 size=0 截断语义——RFC 允许把 sattr.size=0 应用于已存在文件，即 O_TRUNC）。
- **GUARDED**：已存在 → NFS3ERR_EXIST（等价 O_CREAT|O_EXCL 的目录项检查）。
- **EXCLUSIVE**：真正的排它创建，解决"应答丢失后重放误报 EXIST"问题：
  1. 服务器把 8 字节 `verf` **持久存储**在新文件的某个属性里（惯例：塞进 atime+mtime）；
  2. 重放到达时若文件已存在，比较存储的 verf：相同 → 返回成功（这是自己那次创建的重放）；不同 → NFS3ERR_EXIST；
  3. EXCLUSIVE 创建**不带 sattr**，客户端必须随后发 SETATTR 补属性（这次 SETATTR 同时会把 verf 从时间字段里洗掉）；
  4. 服务器不支持 EXCLUSIVE 可返回 NFS3ERR_NOTSUPP（但主流客户端依赖它实现 O_EXCL，建议支持）。
- 返回 `obj` 句柄务必填充，否则客户端要额外 LOOKUP。
- dir_wcc 让客户端更新目录缓存（目录 mtime 变了 → 目录项缓存作废）。

## 9. MKDIR — 创建目录

```
参数: diropargs3 where; sattr3 attributes;
resok/resfail: 同 CREATE
```

已存在 → NFS3ERR_EXIST。其余同 CREATE 常规逻辑。注意 mode 的 SGID 继承、默认 ACL 等由底层文件系统决定。

## 10. SYMLINK — 创建符号链接

```
参数: diropargs3 where;
      struct symlinkdata3 { sattr3 symlink_attributes; nfspath3 symlink_data; }
resok/resfail: 同 CREATE
```

- `symlink_data` 是链接目标（不透明字节串，服务器不校验其指向）。
- 符号链接的 mode 通常被忽略（惯例 0777）。
- 底层不支持符号链接 → NFS3ERR_NOTSUPP（FSINFO 的 FSF3_SYMLINK 位应一致）。

## 11. MKNOD — 创建特殊文件

```
参数: diropargs3 where;
      union mknoddata3 switch (ftype3 type) {
          case NF3CHR:
          case NF3BLK:  struct devicedata3 { sattr3 dev_attributes; specdata3 spec; };
          case NF3SOCK:
          case NF3FIFO: sattr3 pipe_attributes;
          default: void; }
resok/resfail: 同 CREATE
```

- type 只允许 CHR/BLK/SOCK/FIFO；REG/DIR/LNK → NFS3ERR_BADTYPE。
- 建设备文件通常要求 root（且未被 squash），否则 NFS3ERR_PERM/ACCES。
- 不支持 → NFS3ERR_NOTSUPP。**lightnfs 已实现 MKNOD**（`proc_mknod`），以后端能力位 `Cap::kMknod` 决定是否回 NOTSUPP（本地/gluster/lustre/cephfs 后端均置位）。

## 12. REMOVE — 删除文件（unlink）

```
参数: diropargs3 object { dir, name };
resok/resfail: wcc_data dir_wcc
```

- 删除的是**目录项**；若有其他硬链接或有客户端"打开"着文件，数据是否保留取决于服务器本地语义。
- **silly rename 问题**：NFS 无 open 状态，客户端 A 打开文件期间删除它，本地语义要求仍能读写。客户端（不是服务器）用改名为 `.nfsXXXX` 的隐藏文件模拟，最后 close 时再 REMOVE。服务器无需特殊处理，但**不要对 `.nfs*` 名字做特殊限制**。
- 目标是目录：Linux 服务器返回 NFS3ERR_ISDIR（RFC 允许多种处理）。
- 不存在 → NFS3ERR_NOENT（重放会看到这个——DRC 的价值所在）。
- 权限：目录写 + 执行权限；sticky 目录还需属主校验。

## 13. RMDIR — 删除目录

```
参数: diropargs3 object;
resok/resfail: wcc_data dir_wcc
```

- 非空 → **NFS3ERR_NOTEMPTY**（注意不是 EXIST）。
- 目标非目录 → NFS3ERR_NOTDIR。删 `.`/`..` → NFS3ERR_INVAL 或 ACCES/EXIST（RFC 建议 INVAL）。

## 14. RENAME — 改名/移动

```
参数: diropargs3 from; diropargs3 to;
resok/resfail: wcc_data fromdir_wcc; wcc_data todir_wcc;
```

- 必须**原子**（对本地文件系统 rename 的直接映射）。
- 仅限**同一文件系统**内：跨 fsid → NFS3ERR_XDEV。
- to 已存在：类型兼容则原子替换（文件替换文件、空目录替换空目录）；文件→目录 NFS3ERR_ISDIR，目录→文件 NFS3ERR_NOTDIR，目录→非空目录 NFS3ERR_NOTEMPTY（RFC 写作 EXIST 或 NOTEMPTY，Linux 用 NOTEMPTY）。
- from 与 to 指向同一对象：直接成功、不做任何事（POSIX 语义）。
- 把目录移进自己的子孙 → NFS3ERR_INVAL。

## 15. LINK — 创建硬链接

```
参数: nfs_fh3 file; diropargs3 link { dir, name };
resok/resfail: post_op_attr file_attributes; wcc_data linkdir_wcc;
```

- 同一文件系统内（跨 fsid → NFS3ERR_XDEV）；对目录做 LINK 通常拒绝（NFS3ERR_INVAL/ACCES/PERM）。
- 已存在 → NFS3ERR_EXIST。链接数超限 → NFS3ERR_MLINK。
- 不支持硬链接 → NFS3ERR_NOTSUPP（与 FSINFO 的 FSF3_LINK 一致）。

## 16. READDIR — 读目录（仅名字）

```
参数: nfs_fh3 dir; cookie3 cookie; cookieverf3 cookieverf; count3 count;
resok:  post_op_attr dir_attributes; cookieverf3 cookieverf;
        struct dirlist3 {
            entry3 *entries;      /* 链表 */
            bool eof; };
        struct entry3 { fileid3 fileid; filename3 name; cookie3 cookie; entry3 *nextentry; };
resfail: post_op_attr dir_attributes;
```

分页遍历协议：

- 首次调用 cookie=0、cookieverf=全 0；服务器返回若干项，每项带一个 `cookie`（"读到这项之后"的续读位置标记）；客户端用**最后一项的 cookie** 和返回的 `cookieverf` 发起下一轮。
- `count` 限制的是**整个 READDIR 应答消息的 XDR 编码字节数**（不是项数），服务器必须保证编码后不超（XDR 开销：每项 ≈ 8(fileid)+4(名字长度)+名字圆整到 4+8(cookie)+4(nextentry 标志)）。
- `eof=TRUE` 表示目录已到末尾。空目录首轮即 eof（`.`/`..` 是否返回由实现决定——传统服务器返回它们，客户端也预期看到）。
- **cookieverf**：目录的"版本号"。若目录在遍历中途被压缩/重排导致旧 cookie 失效，服务器换 verf；收到旧 verf + 旧 cookie 时返回 **NFS3ERR_BAD_COOKIE**，客户端从头重列。若服务器的 cookie 永远稳定（如基于目录内偏移或稳定序号），可以恒用全 0 verf——**Linux 客户端对 verf 变化很敏感，能用全 0 就用全 0**。
- cookie 值 0、1、2 惯例保留（0=起点，1/2 传统上留给 `.`/`..`），自产 cookie 从 3 或更大开始最稳妥。

## 17. READDIRPLUS — 读目录（含属性与句柄）

```
参数: nfs_fh3 dir; cookie3 cookie; cookieverf3 cookieverf;
      count3 dircount;    /* 仅目录信息部分的字节预算（名字+id+cookie） */
      count3 maxcount;    /* 整个应答的字节预算 */
resok:  post_op_attr dir_attributes; cookieverf3 cookieverf;
        struct entryplus3 { fileid3 fileid; filename3 name; cookie3 cookie;
                            post_op_attr name_attributes;
                            post_op_fh3  name_handle;
                            entryplus3 *nextentry; };
        bool eof;
resfail: post_op_attr dir_attributes;
```

- 等价 READDIR + 每项隐含 LOOKUP+GETATTR，专治 `ls -l` 风暴。
- 双预算：`dircount` 约束"目录骨架"部分（防止一次拉太多项），`maxcount` 约束整个应答。两者都要遵守。
- 每项的属性和句柄都是**可选**的：拿不到（如 lookup 竞争失败）就置 FALSE，客户端会退化为显式 LOOKUP。挂载点交界处惯例不返回句柄。
- 服务器可返回 NFS3ERR_NOTSUPP 表示不支持（客户端会退回 READDIR），但强烈建议支持——对目录密集负载收益巨大。
- cookie/cookieverf 语义与 READDIR 完全一致，且**两个过程应共享同一 cookie 空间**（客户端可能混用）。

## 18. FSSTAT — 文件系统动态统计（df）

```
参数: nfs_fh3 fsroot;
resok:  post_op_attr obj_attributes;
        size3 tbytes, fbytes, abytes;   /* 总/空闲/本用户可用 字节 */
        size3 tfiles, ffiles, afiles;   /* 总/空闲/可用 文件槽（inode） */
        uint32 invarsec;                /* 预计多少秒内不变，0=随时会变 */
resfail: post_op_attr obj_attributes;
```

对应 statfs/statvfs。`abytes ≤ fbytes`（配额/保留块之差）。

## 19. FSINFO — 文件系统静态能力（挂载时必查）

```
参数: nfs_fh3 fsroot;
resok:  post_op_attr obj_attributes;
        uint32 rtmax;     /* READ 最大字节数（硬上限） */
        uint32 rtpref;    /* READ 建议值（≤rtmax） */
        uint32 rtmult;    /* READ 大小建议倍数 */
        uint32 wtmax, wtpref, wtmult;   /* WRITE 同上 */
        uint32 dtpref;    /* READDIR count 建议值 */
        size3  maxfilesize;
        nfstime3 time_delta;            /* 时间戳精度，如 {0,1} 纳秒 */
        uint32 properties;
resfail: post_op_attr obj_attributes;

#define FSF3_LINK        0x0001   /* 支持硬链接 */
#define FSF3_SYMLINK     0x0002   /* 支持符号链接 */
#define FSF3_HOMOGENEOUS 0x0008   /* 整个 fs 内 PATHCONF 结果一致 */
#define FSF3_CANSETTIME  0x0010   /* SETATTR 支持 SET_TO_CLIENT_TIME */
```

- 客户端挂载后第一件事就是对根句柄发 FSINFO，用 rtpref/wtpref 设定 rsize/wsize。现代实现常给 1MB（Linux 服务器默认 rtmax=wtmax=1MB）。
- 声明多大就要扛多大：wtmax=1MB 意味着单条 RPC 消息可达 ~1MB+，接收缓冲与消息上限要匹配。

## 20. PATHCONF — POSIX pathconf 信息

```
参数: nfs_fh3 object;
resok:  post_op_attr obj_attributes;
        uint32 linkmax;           /* 最大硬链接数 */
        uint32 name_max;          /* 最长文件名 */
        bool   no_trunc;          /* TRUE=超长名报错而非截断 */
        bool   chown_restricted;  /* TRUE=只有 root 能 chown */
        bool   case_insensitive;
        bool   case_preserving;
resfail: post_op_attr obj_attributes;
```

典型 UNIX 值：linkmax=32000+，name_max=255，no_trunc=TRUE，chown_restricted=TRUE，case_insensitive=FALSE，case_preserving=TRUE。

## 21. COMMIT — 提交异步写

```
参数: nfs_fh3 file; offset3 offset; count3 count;
resok:  wcc_data file_wcc; writeverf3 verf;
resfail: wcc_data file_wcc;
```

- 把此前 UNSTABLE 写的 [offset, offset+count) 范围刷到稳定存储；**count=0 表示"从 offset 到文件末尾全部"**（惯例 offset=0,count=0 = 整个文件，最常见）。
- 服务器可以刷得更多（整文件 fsync 是最简单的合法实现）。
- 返回的 verf 必须与 WRITE 的 verf 同源：客户端比较，若与写时不同 → 重发未提交数据。
- 客户端触发时机：内存压力、fsync/close（close-to-open 一致性要求 close 前 flush+commit）。

---

## 过程间的横向约束

1. **所有带 wcc_data 的修改操作**（SETATTR、WRITE、CREATE 系、REMOVE 系、RENAME、LINK、COMMIT）：before/after 属性要么原子采样，要么置 FALSE。
2. **凭证一致性**：同一个句柄，不同 uid 请求结果不同；服务器每个请求独立鉴权，不能缓存"上次这个客户端可以"。
3. **STALE 的统一语义**：任何过程收到"对象已不存在"的句柄都返回 NFS3ERR_STALE；收到格式非法句柄返回 NFS3ERR_BADHANDLE。
4. **JUKEBOX/延迟**：操作需要长时间（分层存储上线）时返回 NFS3ERR_JUKEBOX，客户端稍后重试而不报错——也可用于服务器过载背压。
