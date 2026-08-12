# 2. COMPOUND 执行模型与操作总表

## 2.1 只有两个 RPC 过程

```
程序 100003, 版本 4:
  过程 0: NULL      void → void
  过程 1: COMPOUND  COMPOUND4args → COMPOUND4res
```

所有真实功能都是 COMPOUND 里的**操作（operation）**。RPC 层的过程号不再区分功能——抓包和统计要看 COMPOUND 内部。

```c
struct COMPOUND4args {
    utf8str_cs  tag;            /* 调试用标签，服务器原样带回 */
    uint32_t    minorversion;   /* 0 / 1 / 2 */
    nfs_argop4  argarray<>;     /* 操作序列 */
};
struct COMPOUND4res {
    nfsstat4    status;         /* == 最后一个被执行操作的 status */
    utf8str_cs  tag;
    nfs_resop4  resarray<>;     /* 与已执行的操作一一对应 */
};
```

## 2.2 执行语义（必须严格遵守）

1. **顺序执行，遇错即停**：按 argarray 顺序逐个执行；某操作失败（status != NFS4_OK）则后续操作**不执行**，resarray 截止到失败的那个操作。整体 status = 最后执行的操作的 status。
2. **COMPOUND 不是事务**：已执行的操作不回滚。原子性仍然只在单个操作内部（如 RENAME 原子）。
3. **两个句柄寄存器**：执行环境中有 **current filehandle (CFH)** 和 **saved filehandle (SFH)** 两个槽位，操作围绕它们工作：
   - `PUTROOTFH` / `PUTPUBFH` / `PUTFH(fh)`：设置 CFH；
   - `GETFH`：把 CFH 读出来返回给客户端；
   - `LOOKUP(name)`：CFH = CFH 目录下的 name；`LOOKUPP`：CFH = 父目录；
   - `SAVEFH`：SFH ← CFH；`RESTOREFH`：CFH ← SFH；
   - 双目录操作用两个槽位：RENAME/LINK 的源是 SFH、目标是 CFH。
   - COMPOUND 开始时 CFH/SFH 为空；多数操作要求 CFH 存在，否则 NFS4ERR_NOFILEHANDLE。
4. **minorversion 检查**：不支持的 minorversion → NFS4ERR_MINOR_VERS_MISMATCH（整个 COMPOUND 不执行）。4.1+ 要求除少数例外（EXCHANGE_ID、CREATE_SESSION、DESTROY_SESSION、BIND_CONN_TO_SESSION、DESTROY_CLIENTID）外，**SEQUENCE 必须是第一个操作**。
5. 操作数量：4.0 无硬上限（服务器可 NFS4ERR_RESOURCE 拒绝过长的）；4.1 由会话的 ca_maxoperations 协商。
6. 未知操作码 → 按 `ILLEGAL(10044)` 返回 NFS4ERR_OP_ILLEGAL（占位应答，使 resarray 与 argarray 对得上）。

## 2.3 典型 COMPOUND 组合（客户端真实发的样子）

```
# 路径解析 + 取属性（相当于 v3 的 LOOKUP+GETATTR）
{ SEQUENCE; PUTFH(dirfh); LOOKUP("a"); GETFH; GETATTR(bitmap) }

# open(O_RDWR|O_CREAT) 一次往返
{ SEQUENCE; PUTFH(dirfh); OPEN(owner, "f", CREATE, ...); GETFH; GETATTR }

# 读
{ SEQUENCE; PUTFH(filefh); READ(stateid, off, len) }

# rename（跨目录，用双句柄）
{ SEQUENCE; PUTFH(srcdir); SAVEFH; PUTFH(dstdir); RENAME("old","new") }

# stat（属性缓存过期重验）
{ SEQUENCE; PUTFH(fh); GETATTR(change,size,...) }
```

实现提示：虽然理论上客户端可以发任意组合，实践中 Linux/macOS 客户端只发有限的几十种固定模式。先让这些模式跑通，再补全任意组合的正确性。

## 2.4 操作总表

### NFSv4.0 操作（3–39）

| # | 操作 | 一句话语义 | v3 对应 |
|---|------|-----------|---------|
| 3 | ACCESS | 权限查询 | ACCESS |
| 4 | CLOSE | 关闭打开状态（释放 share reservation） | — |
| 5 | COMMIT | 提交异步写 | COMMIT |
| 6 | CREATE | 创建**非常规文件**（目录/链接/设备等；常规文件用 OPEN 创建！） | MKDIR/SYMLINK/MKNOD |
| 7 | DELEGPURGE | 清除客户端声称不再要的委托（CLAIM_DELEGATE_PREV 善后） | — |
| 8 | DELEGRETURN | 归还委托 | — |
| 9 | GETATTR | 按 bitmap 取属性 | GETATTR |
| 10 | GETFH | 读出当前句柄 | （隐含在 LOOKUP 里） |
| 11 | LINK | 硬链接（SFH→CFH 目录） | LINK |
| 12 | LOCK | 加字节区间锁 | NLM LOCK |
| 13 | LOCKT | 测锁（不加） | NLM TEST |
| 14 | LOCKU | 解锁 | NLM UNLOCK |
| 15 | LOOKUP | CFH 下查一个分量 | LOOKUP |
| 16 | LOOKUPP | CFH ← 父目录 | LOOKUP ".." |
| 17 | NVERIFY | 属性**不等**则继续（等则 NFS4ERR_SAME 中止）——做条件 COMPOUND | — |
| 18 | OPEN | 打开/创建常规文件（最复杂的操作） | CREATE(部分) |
| 19 | OPENATTR | 进入命名属性目录 | — |
| 20 | OPEN_CONFIRM | 确认首次 open-owner（**4.1 废除**） | — |
| 21 | OPEN_DOWNGRADE | 降低 share access/deny（对应部分 close） | — |
| 22 | PUTFH | CFH ← 参数句柄 | （每个 v3 请求的 fh 参数） |
| 23 | PUTPUBFH | CFH ← public 句柄 | — |
| 24 | PUTROOTFH | CFH ← 伪文件系统根 | （MOUNT 的替代起点） |
| 25 | READ | 读（带 stateid） | READ |
| 26 | READDIR | 列目录（带属性 bitmap，兼并了 READDIRPLUS） | READDIR(PLUS) |
| 27 | READLINK | 读符号链接 | READLINK |
| 28 | REMOVE | 删除目录项（文件和目录共用！无 RMDIR） | REMOVE/RMDIR |
| 29 | RENAME | 改名（SFH 源目录，CFH 目标目录） | RENAME |
| 30 | RENEW | 显式续租（**4.1 废除**，由 SEQUENCE 兼任） | — |
| 31 | RESTOREFH | CFH ← SFH | — |
| 32 | SAVEFH | SFH ← CFH | — |
| 33 | SECINFO | 查询某名字需要的安全 flavor | — |
| 34 | SETATTR | 设属性（带 stateid，truncate 需有效打开态） | SETATTR |
| 35 | SETCLIENTID | 建立客户端 ID（**4.1 由 EXCHANGE_ID 取代**） | — |
| 36 | SETCLIENTID_CONFIRM | 确认客户端 ID（同上废除） | — |
| 37 | VERIFY | 属性**相等**则继续（不等则 NFS4ERR_NOT_SAME 中止） | — |
| 38 | WRITE | 写（带 stateid；stable_how 语义同 v3） | WRITE |
| 39 | RELEASE_LOCKOWNER | 释放 lock-owner 状态（4.0 专用） | — |

### NFSv4.1 新增（40–58）

| # | 操作 | 说明 |
|---|------|------|
| 40 | BACKCHANNEL_CTL | 调整回传通道属性 |
| 41 | BIND_CONN_TO_SESSION | 把新 TCP 连接绑进既有会话（trunking/重连） |
| 42 | EXCHANGE_ID | 建立 clientid（取代 SETCLIENTID） |
| 43 | CREATE_SESSION | 创建会话（协商槽表、缓冲、回传通道） |
| 44 | DESTROY_SESSION | 销毁会话 |
| 45 | FREE_STATEID | 释放已无用的 stateid |
| 46 | GET_DIR_DELEGATION | 目录委托 |
| 47 | GETDEVICEINFO | pNFS：取设备信息 |
| 48 | GETDEVICELIST | pNFS：列设备（4.2 弃用） |
| 49 | LAYOUTCOMMIT | pNFS：提交布局内写入的可见性 |
| 50 | LAYOUTGET | pNFS：取布局 |
| 51 | LAYOUTRETURN | pNFS：还布局 |
| 52 | SECINFO_NO_NAME | 对 CFH 本身查安全 flavor（含 PUTROOTFH 场景） |
| 53 | SEQUENCE | 会话序号/槽位（几乎每个 COMPOUND 的第一个操作） |
| 54 | SET_SSV | 会话安全值（SP4_SSV 模式用，实践罕见） |
| 55 | TEST_STATEID | 批量检验 stateid 是否仍有效 |
| 56 | WANT_DELEGATION | 显式申请委托 |
| 57 | DESTROY_CLIENTID | 销毁 clientid |
| 58 | RECLAIM_COMPLETE | 声明"我的重建工作做完了"（恢复期边界） |

### NFSv4.2 新增（59–71）

| # | 操作 | 说明 |
|---|------|------|
| 59 | ALLOCATE | 预分配空间（fallocate） |
| 60 | COPY | 服务器端拷贝（同服/跨服） |
| 61 | COPY_NOTIFY | 跨服拷贝授权 |
| 62 | DEALLOCATE | 打洞（punch hole） |
| 63 | IO_ADVISE | posix_fadvise 语义提示 |
| 64 | LAYOUTERROR | pNFS：报告数据服务器 IO 错误 |
| 65 | LAYOUTSTATS | pNFS：报告 IO 统计 |
| 66 | OFFLOAD_CANCEL | 取消异步拷贝 |
| 67 | OFFLOAD_STATUS | 查询异步拷贝进度 |
| 68 | READ_PLUS | 读（结果可表达空洞，稀疏文件友好） |
| 69 | SEEK | SEEK_HOLE/SEEK_DATA |
| 70 | WRITE_SAME | 按模式块重复写（ADB） |
| 71 | CLONE | reflink 式克隆（同文件系统，字节范围） |

### 回调操作（服务器 → 客户端，CB_COMPOUND 内）

| # | 操作 | 版本 | 说明 |
|---|------|------|------|
| 3 | CB_GETATTR | 4.0+ | 向持写委托的客户端查真实 size/change |
| 4 | CB_RECALL | 4.0+ | 召回委托 |
| 5 | CB_LAYOUTRECALL | 4.1 | 召回 pNFS 布局 |
| 6 | CB_NOTIFY | 4.1 | 目录变更通知（目录委托） |
| 7 | CB_PUSH_DELEG | 4.1 | 主动塞给客户端一个委托 |
| 8 | CB_RECALL_ANY | 4.1 | "随便还我 N 个可召回对象"（资源压力） |
| 9 | CB_RECALLABLE_OBJ_AVAIL | 4.1 | 之前拒的委托现在有了 |
| 10 | CB_RECALL_SLOT | 4.1 | 收缩会话槽表 |
| 11 | CB_SEQUENCE | 4.1 | 回传通道的序号操作（第一个） |
| 12 | CB_WANTS_CANCELLED | 4.1 | 取消 WANT_DELEGATION 登记 |
| 13 | CB_NOTIFY_LOCK | 4.1 | 等待的锁可用了 |
| 14 | CB_NOTIFY_DEVICEID | 4.1 | pNFS 设备变更 |
| 15 | CB_OFFLOAD | 4.2 | 异步 COPY 完成通知 |

## 2.5 与 v3 语义对齐时的注意点

- **CREATE 不建常规文件**：常规文件只能 OPEN(OPEN4_CREATE) 创建——因为创建必须同时建立打开状态。
- **REMOVE 合并了 RMDIR**：对目录 REMOVE 即 rmdir（非空 → NFS4ERR_NOTEMPTY）。
- **READDIR 自带属性**：参数含 attr bitmap，等价 v3 READDIRPLUS；不再有独立 PLUS 操作。每个目录项若取不到属性，可在该项属性里返回 `rdattr_error`。
- **VERIFY/NVERIFY** 是 v4 独有的"条件执行"原语：客户端可拼出"若 change 未变才执行后续操作"的 COMPOUND，替代部分 WCC 用法。
- WRITE 的 stable_how/verifier 语义与 v3 完全一致（见 nfsv3 分册 7.5 节），COMMIT 亦然。
