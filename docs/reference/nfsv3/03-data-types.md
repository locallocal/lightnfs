# 3. NFSv3 基本数据类型与结构

本文列出 RFC 1813 第 2.5、2.6 节定义的全部核心结构。这些结构在 21 个过程中反复出现，是实现编解码层的基础。

## 3.1 常量

```c
#define NFS3_FHSIZE         64   /* 文件句柄最大字节数 */
#define NFS3_COOKIEVERFSIZE  8   /* READDIR cookie verifier */
#define NFS3_CREATEVERFSIZE  8   /* EXCLUSIVE CREATE verifier */
#define NFS3_WRITEVERFSIZE   8   /* WRITE/COMMIT verifier */
#define NFS_PORT          2049   /* 惯用端口 */
#define NFS_PROGRAM     100003
#define NFS_V3               3
```

## 3.2 状态码 nfsstat3

每个过程结果的第一个字段都是 `nfsstat3 status`。完整错误码见 [08-errors.md](08-errors.md)。**只有 status == NFS3_OK(0) 时才编码"成功分支"，否则编码"失败分支"**（失败分支通常仍带 post-op 属性 / WCC 数据，便于客户端维持缓存）。

## 3.3 文件对象类型 ftype3

```c
enum ftype3 {
    NF3REG  = 1,  /* 常规文件 */
    NF3DIR  = 2,  /* 目录 */
    NF3BLK  = 3,  /* 块设备 */
    NF3CHR  = 4,  /* 字符设备 */
    NF3LNK  = 5,  /* 符号链接 */
    NF3SOCK = 6,  /* UNIX domain socket */
    NF3FIFO = 7   /* 命名管道 */
};
```

## 3.4 文件句柄 nfs_fh3

```c
struct nfs_fh3 { opaque data<NFS3_FHSIZE>; };   /* 0..64 字节 */
```

- 对客户端完全**不透明**：只能缓存、比较、原样带回，不得解析。
- 由服务器构造，必须满足：
  1. **唯一性**：同一时刻不同对象句柄不同；
  2. **持久性**：同一对象的句柄跨服务器重启保持有效（这是"持久句柄"要求；v3 没有 v4 的 volatile 句柄概念）；
  3. 对象删除后句柄失效，访问返回 NFS3ERR_STALE。
- 长度为 0 的句柄有特殊惯例：WebNFS 的"public filehandle"（RFC 2054，可不支持）。
- 典型构造：`{fsid, inode 号, generation 计数}`（generation 防止 inode 复用导致旧句柄指向新文件）。设计讨论见 [09-implementation-notes.md](09-implementation-notes.md)。

## 3.5 时间 nfstime3

```c
struct nfstime3 { uint32 seconds; uint32 nseconds; };
```

自 1970-01-01 UTC 起。nseconds 必须 < 10^9。注意 seconds 是 **uint32**（2106 年问题；RFC 如此规定，Linux 实现按无符号处理）。

## 3.6 文件属性 fattr3（对应 stat）

```c
struct specdata3 { uint32 specdata1; uint32 specdata2; };  /* major, minor */

struct fattr3 {
    ftype3    type;    /* 对象类型 */
    mode3     mode;    /* 低 12 位权限：0x800 SUID, 0x400 SGID, 0x200 sticky,
                          0x100..0x001 = rwxrwxrwx */
    uint32    nlink;   /* 硬链接数 */
    uid3      uid;
    gid3      gid;
    size3     size;    /* 字节大小；符号链接=路径长度；设备等未定义 */
    size3     used;    /* 实际占用字节（约 = st_blocks*512），稀疏文件 < size */
    specdata3 rdev;    /* 设备号，仅 BLK/CHR 有意义 */
    uint64    fsid;    /* 文件系统 ID */
    fileid3   fileid;  /* 文件系统内唯一 ID（inode 号） */
    nfstime3  atime, mtime, ctime;
};
```

要点：

- `fileid` 在一个 fsid 内必须唯一，且同一对象保持稳定——客户端（如 Linux）用它做 inode 号；readdir 去重、`find -samefile`、硬链接检测都依赖它。
- `ctime` 是属性变更时间，不可由客户端设置（SETATTR 改任何东西都会推进 ctime）。
- mode 位没有类型位（类型在 `type` 字段），只有 12 位权限。

## 3.7 post_op_attr 与 pre_op_attr（缓存辅助）

```c
union post_op_attr switch (bool attributes_follow) {
    case TRUE:  fattr3 attributes;
    case FALSE: void;
};

struct wcc_attr {           /* 修改前属性的最小子集 */
    size3    size;
    nfstime3 mtime;
    nfstime3 ctime;
};

union pre_op_attr switch (bool attributes_follow) {
    case TRUE:  wcc_attr attributes;
    case FALSE: void;
};

struct wcc_data {           /* Weak Cache Consistency 数据 */
    pre_op_attr  before;
    post_op_attr after;
};
```

- 几乎所有过程的结果里都带 `post_op_attr`（成功和失败分支都带），让客户端免费刷新属性缓存。**返回属性总是可选的**（attributes_follow=FALSE 合法），但好的服务器应尽量返回。
- 所有修改目标对象/目录的过程返回 `wcc_data`：客户端比较 `before` 与自己缓存的属性——一致则说明"两次观测之间没有别人修改过"，本地缓存可以无缝衔接；不一致则作废缓存。**before 和 after 必须是同一次操作原子采样的**，否则宽松返回 FALSE 更安全（详见 [07-caching-consistency.md](07-caching-consistency.md)）。

```c
union post_op_fh3 switch (bool handle_follows) {
    case TRUE:  nfs_fh3 handle;
    case FALSE: void;
};
```

CREATE/MKDIR/SYMLINK/MKNOD 返回新对象句柄用它（可选，但不返回会迫使客户端再 LOOKUP 一次，务必返回）。

## 3.8 可设置属性 sattr3（SETATTR/CREATE 等的入参）

每个字段独立可选：

```c
enum time_how {
    DONT_CHANGE        = 0,
    SET_TO_SERVER_TIME = 1,   /* 用服务器当前时间（等价 utimes(NULL)） */
    SET_TO_CLIENT_TIME = 2    /* 用请求中携带的时间 */
};

union set_mode3 switch (bool set_it) { case TRUE: mode3 mode; default: void; };
union set_uid3  switch (bool set_it) { case TRUE: uid3  uid;  default: void; };
union set_gid3  switch (bool set_it) { case TRUE: gid3  gid;  default: void; };
union set_size3 switch (bool set_it) { case TRUE: size3 size; default: void; };
union set_atime switch (time_how set_it) { case SET_TO_CLIENT_TIME: nfstime3 atime; default: void; };
union set_mtime switch (time_how set_it) { case SET_TO_CLIENT_TIME: nfstime3 mtime; default: void; };

struct sattr3 {
    set_mode3 mode;
    set_uid3  uid;
    set_gid3  gid;
    set_size3 size;    /* 设 size 即 truncate/extend */
    set_atime atime;
    set_mtime mtime;
};
```

## 3.9 目录操作通用参数 diropargs3

```c
struct diropargs3 {
    nfs_fh3   dir;    /* 目录句柄 */
    filename3 name;   /* 单个名字分量 */
};
```

`filename3` 约束（服务器应校验）：

- 不得为空（NFS3ERR_ACCES 或 INVAL）；
- 不得含 `/`（UNIX 服务器）和 NUL 字节；
- `.` 和 `..` 仅在 LOOKUP 中有惯例语义（当前目录/父目录），其他过程应拒绝；
- 超过底层限制返回 NFS3ERR_NAMETOOLONG；
- 协议不规定字符集（字节串语义），实现按原样传递即可。
