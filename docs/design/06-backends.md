# 6. 后端实现：本地文件系统（v1）与未来后端映射验证

## 6.1 本地后端（backend_local）总体

导出本机挂载的一棵目录树（ext4/XFS/Btrfs，也天然覆盖"POSIX 挂载的 Lustre"基础形态）。核心手段：

- **句柄反查**：`name_to_handle_at(2)` / `open_by_handle_at(2)`（需要 CAP_DAC_READ_SEARCH）——内核原生的 `{fhandle}` 正是"持久对象标识"，直接作为 `ObjId` 内容；
- **文件 IO**：io_uring（pread/pwrite/fsync/statx）；
- **路径类操作**（openat/renameat2/unlinkat/linkat/mkdirat/getdents64…）：无 uring 原语的走 `rt::offload()`。

### ObjId 编码

```
ObjId = { fsid_hint(0—由 Backend 级 fsid 表达，不进 ObjId),
          kernel_fhandle: handle_bytes ≤ 40B（ext4/xfs 实际 8–28B） }
```

- P1/P2 由内核保证（fhandle 内含 inode+generation）；总长远小于 51B 上限。
- 降级模式（无特权运行）：`ObjId = {ino(8B), gen(4B)}` + 进程内 ino→路径缓存，接受外部 rename 下的 ESTALE 误报（nfsv3 分册 9.1 方案 A 降级路径）；能力位与文档明示限制。

### Object 实现

```cpp
class LocalObject final : public Object {
    LocalBackend&   be_;
    ObjId           oid_;
    FdCache::Ref    fd_;      // 惰性：首次需要 fd 的操作时 open_by_handle_at
};
```

- `getattr` → `uring_statx(fd 或 AT_EMPTY_PATH)`，`Attr::change` 取 `STATX_CHANGE_COOKIE`（内核 ≥6.6 且 fs 支持 → 置 kNativeChange；否则 core 合成，见 05 分册 5.6）。
- `lookup` → `openat(dirfd, name, O_PATH|O_NOFOLLOW)` + `name_to_handle_at` → 新 LocalObject。O_NOFOLLOW + `name` 禁 `/` 与 NUL：路径逃逸防御双保险（core 已校验一次）。
- 目录修改类 → 对应 *at 系（offload）；`rename` 用 `renameat2(0)`。
- `create` EXCLUSIVE：`O_CREAT|O_EXCL` 成功后 verifier 写入 atime/mtime（`utimensat`）；EEXIST 时读回比对——与协议语义严格对齐（nfsv3/04 §8）。
- `readdir`：`getdents64`（offload，批量 64KiB），cookie = `d_off`（ext4/xfs 为稳定 hash/位置，满足 5.7 契约）；DirPage 尽力带 attr（batch statx）与 oid（batch name_to_handle_at）供 READDIRPLUS——两个 batch 都可配置关闭以换元数据延迟。

### 6.2 IO 与稳定性

| 接口调用 | 实现 |
|----------|------|
| write(kUnstable) | `uring_pwrite`（页缓存即返回） |
| write(kDataSync) | pwrite + `uring_fsync(datasync)`（或 RWF_DSYNC pwritev2 一次完成） |
| write(kFileSync) | pwrite + fsync |
| commit(off,len) | `sync_file_range` 不满足持久语义，**用 fdatasync**（len 参数仅作提示）；len=0 → fdatasync |
| seek/allocate/deallocate | `lseek(SEEK_HOLE/DATA)`(offload) / `fallocate(0 / PUNCH_HOLE)` → 置 kSparseOps |
| clone | `ioctl(FICLONERANGE)`，仅 XFS(reflink=1)/Btrfs → 运行时探测置 kCloneRange |
| copy_range | `copy_file_range`(offload 或 uring ≥5.19) → 置 kCopyRange |

write verifier 语义（nfsv3/07 红线）由 core 的 boot epoch 承担；本地后端唯一义务：commit/kFileSync 返回成功后数据必须真在盘上（fsync 错误必须上报——fsync 返回 EIO 后**标记该文件后续 commit 恒错**，不吞 writeback 错误）。

### 6.3 fd 缓存（匿名 IO 的关键）

```cpp
class FdCache {          // 分片 LRU：ObjId → {fd, refcnt, last_use}
    Task<Result<Ref>> acquire(const ObjId&, int flags);   // 命中直返；未命中 open_by_handle_at
};
```

- v3 READ/WRITE（OpenCtx.open==nullptr）经它拿 fd；v4 OPEN 的 OpenState 直接持独立 fd（不占缓存）。
- 容量默认 4096，水位驱逐；驱逐只关 fd 不失效 ObjId（下次再开）。
- O_RDWR 打开一次可服务读写；只读文件系统降级 O_RDONLY。

### 6.4 身份执行

本地后端两种模式（配置选）：

1. **权限位自查（默认）**：进程以 root 跑，每操作用 `Cred` 对 statx 结果做 POSIX 权限计算（含属主放宽惯例），错判风险：ACL/富权限文件系统 → 提供 `access()` 走 `faccessat2(AT_EACCESS)`（offload、切 fsuid）兜底校验的可选严格模式；
2. **setfsuid/setfsgid**：offload 线程内切换 fsuid 后执行（线程级生效，同步原语保证串行）——语义最准，吞吐受 offload 串行度限制。

v1 默认模式 1；接口上不体现差异（都在后端内部）。

## 6.5 映射验证：Lustre 后端（未来）

| Backend API | Lustre 映射 | 备注 |
|-------------|------------|------|
| ObjId | FID（`lustre_fid` 16B，集群持久唯一） | `llapi_fid2path`/`llapi_open_by_fid`——比本地 fhandle 更干净 |
| resolve | `llapi_open_by_fid` | P1/P2 天然满足 |
| IO | POSIX 挂载点上的 uring pread/pwrite | 大条带顺序 IO 直通 |
| change | Lustre changelog/版本 → kNativeChange | 多网关一致性可用 |
| statfs/limits | `llapi_obd_statfs`；条带感知的 pref_read/pref_write | FSINFO 通告条带对齐值 |
| native_locks | flock 语义经 MDS 全局一致 → kByteLocks 可置 | 多网关锁下沉的第一个真实用户 |
| kJukebox | HSM released 文件 → kJukebox | READ 触发 restore 返回 kJukebox |

结论：接口无缺口；Lustre 特有优化（statahead、组锁）可全部藏在后端内。

## 6.6 映射验证：GlusterFS 后端 ✅ 已实现（2026-09-03，plan doc 10 §5.3）

| Backend API | libgfapi 映射（实现） | 备注 |
|-------------|--------------|------|
| Backend::start/stop | `glfs_new` + `glfs_set_volfile_server`(每个 server) + `glfs_set_logging` + `glfs_init`，再 `glfs_h_lookupat(NULL, subdir)` 取导出根（offload kHeavy）；stop 先关锁 glfd/fd 缓存/对象缓存再 `glfs_fini` | 每导出一个 glfs_t；工厂构造不连接，`start()` 才连（配置加载不阻塞于集群） |
| ObjId | 标记字节 `3` + 16B GFID（`glfs_h_extract_handle`） → kStableHandles | 17B；`GlusterBackend::gfid_from_oid` 是解析边界（fuzz 目标 `objid_gluster`） |
| resolve | `glfs_h_create_from_handle` → `glfs_object*`，前置分片 LRU 对象句柄缓存（容量 = fd_cache） | ENOENT/ESTALE/EINVAL → ESTALE；ObjPtr 共享 `ObjHandle`（最后一个用户 `glfs_h_close`） |
| lookup/creat/… | `glfs_h_lookupat/h_creat/h_mkdir/h_mknod/h_symlink/h_unlink/h_rename/h_link/h_readlink/h_setattrs/h_truncate/h_stat/h_statfs` | `..` 在导出根钳到根；unlink/rmdir 先 lookup 定类型（`glfs_h_unlink` 对目录也会 rmdir，REMOVE 目录须回 EISDIR）；创建后若砖块 umask 改了 mode 则补一次 setattrs |
| 身份 | `glfs_setfsuid/setfsgid/setfsgroups` 在 offload 线程逐调用设置、调用后复位 | 砖块 posix-acl 判定 → **kNativeAccess**：`access()` = 每个所需 POSIX 模式一次 `glfs_h_access`（最多 3 次） |
| open/IO | v4 OPEN：`glfs_h_open` 以调用者身份 → `GlusterOpenState`（EACCES 即 OPEN 的答案，不降级）；匿名 IO：每对象 glfd 缓存（读→写升级、LRU 驱逐、以网关身份打开，门禁 = 原生 access + v3 属主放宽）；`glfs_pread/pwritev`，kDataSync/kFileSync → `glfs_fdatasync/fsync`；commit = fdatasync，失败粘性 poison（§6.2） | 全部 offload；数据路径 kLight，同步/分配/拷贝 kHeavy |
| readdir | `glfs_h_opendir` + `glfs_seekdir(cookie)` + `glfs_xreaddirplus_r(STAT\|HANDLE)`，d_off 作 cookie；每页一个目录 glfd | 富化：attr 取 xstat 的 stat，oid 取 xstat 对象的 GFID（对象随 `glfs_free(xstat)` 释放） |
| change | 无原生 → 不置 kNativeChange，`change = ctime` 合成（05 §5.6）；v4.2 `change_attr_type = TIME_METADATA` | 多网关部署的 CTO 依赖 ctime 精度（文档限制） |
| v4.2 | `glfs_lseek(SEEK_DATA/HOLE)` / `glfs_fallocate(0)` / `glfs_discard` → kSparseOps；`glfs_copy_file_range`（EXDEV/EOPNOTSUPP/ENOSYS/EINVAL 回落 pread/pwrite）→ kCopyRange | 无 CLONE |
| locks | `GlusterLockMgr`：每 (文件, lock-owner) 一个 glfd（网关身份 O_RDWR，EACCES 回落 O_RDONLY）+ `glfs_fd_set_lkowner`，`glfs_posix_lock(F_SETLK)`；冲突 EAGAIN；`test()` 用探测 owner 的 F_GETLK；`release()` 全量解锁并关 glfd → **kByteLocks**，`native_locks()` 交给状态层下推（07 册 / plan doc 10 §5.3） | `native_locks=false` 关闭 |
| jukebox | ENOTCONN/ETIMEDOUT/ENETDOWN/ENETUNREACH/EHOSTUNREACH/EHOSTDOWN → `kJukebox`（v3 JUKEBOX / v4 DELAY）→ **kJukebox** | 砖块重连/仲裁丢失期间客户端重试而非报错；`jukebox=false` 时回 EIO |

**绑定方式**：`backend/gfapi.hpp` 定义一张 47 项函数表（签名取自 GlusterFS 11 的
`glfs.h`/`glfs-handles.h`，不透明结构体按真名在全局命名空间声明），`gfapi.cpp` 在
`start()` 时 `dlopen("libgfapi.so.0")` + `dlvsym`（默认版本 `GFAPI_x.y`，回落 `dlsym`）填表
——二进制无构建期 GlusterFS 依赖；`scripts/check_gfapi_abi.sh` 在有头文件的主机上把每
个成员与真实声明做编译期比对（已对 11.2 头文件通过）。测试用 `tests/gfapi_fake.cpp`
在本地目录上实现同一张表（线程局部 fsuid、带世代号的 16B 句柄、xstat 所有权、按
lk-owner 键的 posix 锁表、可注入传输错误），`tests/test_gluster.cpp` 由此把整条后端
逻辑跑在 ctest 里；真实集群验收 = `scripts/accept_gluster.sh`。

结论（验证后）：接口无缺口——唯一改动是两处可选扩展：`LockMgr::release(Object&, owner)`
（默认全区间 unlock；gluster 覆写为关 glfd）与 `BackendFactory::virtual_path`（集群后端
的 `path` 只是挂载名）。摩擦点确如预期是 libgfapi 阻塞调用：offload 池并发度 =
该后端吞吐上限（`[server] offload_threads` 是它的主要调优旋钮）；`glfs_*_async` 族改造
留作后续。

## 6.7 后端选择与配置（示例）

```toml
[[export]]
path      = "/export/data"        # local: 根路径
backend   = "local"
fsid      = 1
clients   = ["192.168.0.0/24"]
squash    = "root"                # none|root|all
readonly  = false
[export.local]
fd_cache  = 4096
identity  = "check"               # check|setfsuid

# GlusterFS（已实现，plan doc 10 §5.3）：path 只是客户端挂载名，树在卷里
[[export]]
path      = "/gluster"
backend   = "gluster"
fsid      = 2
clients   = ["192.168.0.0/24"]
squash    = "none"                # 身份透传给砖块鉴权（kNativeAccess）
[export.gluster]
volume         = "vol0"
servers        = "gs1,gs2:24007"  # volfile 服务器，逗号分隔，可 host:port / [v6]:port
subdir         = "/exports/a"     # 卷内导出根，默认 "/"
# transport    = "tcp"            # tcp | rdma | unix
# log_file     = "/var/log/lightnfs/gfapi.log"   # 空 = libgfapi 默认（非 root 通常不可写）
# log_level    = 4                # gluster 日志级别 0..9（4 = ERROR，7 = INFO）
# fd_cache     = 1024             # 匿名 IO glfd 缓存 / 对象句柄缓存容量
# readdir_enrich = true           # xreaddirplus 带 stat + 句柄（READDIRPLUS 免逐项 lookup）
# jukebox      = true             # 传输类错误 → JUKEBOX/DELAY（false → EIO）
# native_locks = true             # glfs_posix_lock 下推（多网关锁一致性）

# 未来：
# backend = "lustre";   [export.lustre]  mount="/mnt/lustre"
```
