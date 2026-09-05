# 6. 后端实现：本地文件系统与集群后端（Lustre / GlusterFS / CephFS）

## 6.1 本地后端（backend_local）总体

导出本机挂载的一棵目录树（ext4/XFS/Btrfs，也天然覆盖"POSIX 挂载的 Lustre"基础形态）。核心手段：

- **句柄反查**：`name_to_handle_at(2)` / `open_by_handle_at(2)`（需要 CAP_DAC_READ_SEARCH）——内核原生的 `{fhandle}` 正是"持久对象标识"，直接作为 `ObjId` 内容；
- **文件 IO**：io_uring（pread/pwrite/fsync/statx）；
- **路径类操作**（openat/renameat2/unlinkat/linkat/mkdirat/getdents64…）：无 uring 原语的走 `rt::offload()`。

### ObjId 编码

```
ObjId = { fsid_hint(0—由 Backend 级 fsid 表达，不进 ObjId),
          kernel_fhandle: handle_bytes ≤ 45B（= ObjId::kMax − 6，ext4/xfs 实际 8–28B） }
```

- P1/P2 由内核保证（fhandle 内含 inode+generation）；总长远小于 51B 上限。
- 降级模式（无特权运行，`handles = "fallback"` 或 `auto` 探测失败）：`ObjId = {tag(1B), dev(8B), ino(8B), gen(4B)}` = 21B，gen 取 STATX_BTIME（无 btime 的文件系统如 tmpfs 退为进程内计数，句柄不跨重启稳定）+ 进程内 ino→路径缓存，接受外部 rename 下的 ESTALE 误报（nfsv3 分册 9.1 方案 A 降级路径）；不置 kStableHandles，文档明示限制。

### Object 实现

```cpp
class LocalObject final : public Object {
    LocalBackend&   be_;
    ObjId           oid_;
    FdCache::Ref    fd_;      // 惰性：首次需要 fd 的操作时 open_by_handle_at
};
```

实现中有两层缓存：O_PATH 解析缓存（`resolve()` 命中免去 open_by_handle_at 往返，
`lightnfs_fdcache_path_*` 指标）与本节的数据 fd 缓存。

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

本地后端三种模式（`[export.local] identity`）：

1. **`check`（默认）权限位自查**：每操作用 `Cred` 对 statx 结果做 POSIX 权限计算（含属主放宽惯例）；错判风险在 ACL/富权限文件系统；
2. **`strict`**：在 1 的基础上，`access()` 授予的每一位再用 `faccessat2(AT_EACCESS)`（offload、切 fsuid）复核，捕获 ACL 等 mode 位之外的策略；
3. **`setfsuid`（需 root）**：offload 线程内切换 fsuid/fsgid 后执行，内核权威判定——语义最准，吞吐受 offload 并发度限制。

接口上不体现差异（都在后端内部）；lustre 后端继承同一套，gluster/cephfs 则把 `Cred` 交给存储侧判定（§6.6/§6.8）。

## 6.5 映射验证：Lustre 后端 ✅ 已实现（2026-09-04）

Lustre 客户端挂载就是一棵 POSIX 树，因此 **`LustreBackend : LocalBackend`**：本地后端去掉
`final`，开放三处虚函数接缝——句柄编解码 `oid_from_fd`、按句柄打开 `open_oid`、以及经由
后者的数据 fd 门禁；IO、命名空间操作、fd/O_PATH 缓存、粘性 poison、身份模式全部继承，
无重复代码。

| Backend API | Lustre 映射（实现） | 备注 |
|-------------|--------------|------|
| ObjId | 标记字节 `4` + 16B FID（`lu_fid` seq/oid/ver，取自 `name_to_handle_at` 返回的 FILEID_LUSTRE 句柄前 16B；回落 `LL_IOC_PATH2FID`）→ **kStableHandles** | 17B；`LustreBackend::fid_from_oid` 是解析边界（fuzz 目标 `objid_lustre`）；FID_ZERO 视为 ESTALE |
| resolve | `openat(<mount>/.lustre/fid/0xseq:0xoid:0xver)`（`llapi_open_by_fid` 的机制），**不需要 CAP_DAC_READ_SEARCH**；ENOENT/EINVAL → ESTALE | 挂载根沿父目录上溯到 st_dev 变化处自动探测，或 `mount=` 指定（bind mount 场景）；启动时对根 FID 做一次往返，失败即拒绝启动 |
| IO | 继承本地：io_uring pread/pwrite/writev，分片 fd 缓存（读→写升级），commit = fdatasync + 粘性 poison | 大条带顺序 IO 直通 |
| change | ctime 合成（05 §5.6），**不置 kNativeChange** | 时间戳由 MDT 签发，跨网关一致；`LL_IOC_DATA_VERSION` 每条带一次 OST 往返且只覆盖数据，不上 GETATTR 路径 |
| statfs/limits | `fstatvfs`（Lustre 聚合 OST）；`pref_read/pref_write` = 导出根默认条带大小（`lustre.lov` xattr：V1/V3 直接取，复合布局取首组件），钳到 [4K, max_read/max_write] | FSINFO 通告条带对齐值 |
| native_locks | `LustreLockMgr`：每 (文件, lock-owner) 一个 fd + `F_OFD_SETLK/F_OFD_GETLK`；`-o flock` 挂载时由 MDS 全局仲裁 → **kByteLocks**，`native_locks()` 交给状态层下推（07 册 §7.6 同一条链路） | `localflock` 或非 Lustre 文件系统上仅主机内有效；`test()` 用新 fd 探测（OFD 不报告持有者，owner 置空 = "别人"）；`release()` 全区间解锁并关 fd；`native_locks=false` 关闭 |
| kJukebox | 数据 fd 打开后 `LL_IOC_HSM_STATE_GET`：HS_RELEASED → 若无进行中动作则 `LL_IOC_HSM_REQUEST(RESTORE)` 一次，返回 `kJukebox`（v3 JUKEBOX / v4 DELAY；v4 OPEN 也回 DELAY 而非降级为匿名路径）→ **kJukebox** | 避免 offload / io_uring 工作线程阻塞在内核隐式 restore 上；只门禁常规文件；无 HSM 的客户端（ENOTTY）不门禁；`hsm=false` 关闭 |
| 身份 | 继承 `identity = check \| strict \| setfsuid` | Lustre 在 MDS 侧按 fsuid 判权，root 网关宜用 setfsuid |

**绑定方式**：`backend/lustre/llapi.{hpp,cpp}` 直接对内核客户端说话——ioctl 号、结构体布局与常量
抄自 `linux/lustre/lustre_user.h`（稳定 uapi），**无 liblustreapi 构建/运行依赖**；
`scripts/check_llapi_abi.sh` 在有该头文件的主机上把每个常量与结构体尺寸 static_assert
（本开发机无 Lustre，CI 步骤在无头文件时跳过）。`llapi::Ops` 是唯一的内核接触面：
`tests/llapi_fake.cpp` 在本地目录上实现它（FID 由 inode+btime 派生并用 O_PATH fd 钉住，
open_by_fid 经 `/proc/self/fd` 重开——重命名后仍可解析、删除后 ENOENT，与真实语义一致；
HSM 状态/restore/条带大小是测试旋钮），`tests/test_lustre.cpp` 10 个用例跑 05 §5.9 检查表
（句柄 P1/P2/ESTALE、跨重启与重命名、readdir 富化携带 FID、匿名/open-state IO、HSM
released → JUKEBOX → restore 后可读、原生锁、条带 → limits、挂载根探测与拒绝路径、
配置工厂——真实内核绑定在本机 tmp 目录上被正确拒绝）。真实挂载验收 =
`scripts/accept_lustre.sh`。

结论（验证后）：接口仍无缺口；第三个后端带来的唯一结构改动是本地后端的可继承化
（两个虚函数 + 受保护的缓存/能力位成员）。边界：change 非原生（跨网关 CTO 依赖 MDT
时间戳精度）；描述符已在 fd 缓存中的文件被释放时，后续 IO 仍像原生客户端一样阻塞在
restore 上（门禁只在打开时刻）；OFD 锁的探测无法辨认持有者身份。

## 6.6 映射验证：GlusterFS 后端 ✅ 已实现（2026-09-03）

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
| locks | `GlusterLockMgr`：每 (文件, lock-owner) 一个 glfd（网关身份 O_RDWR，EACCES 回落 O_RDONLY）+ `glfs_fd_set_lkowner`，`glfs_posix_lock(F_SETLK)`；冲突 EAGAIN；`test()` 用探测 owner 的 F_GETLK；`release()` 全量解锁并关 glfd → **kByteLocks**，`native_locks()` 交给状态层下推（07 册 §7.6） | `native_locks=false` 关闭 |
| jukebox | ENOTCONN/ETIMEDOUT/ENETDOWN/ENETUNREACH/EHOSTUNREACH/EHOSTDOWN → `kJukebox`（v3 JUKEBOX / v4 DELAY）→ **kJukebox** | 砖块重连/仲裁丢失期间客户端重试而非报错；`jukebox=false` 时回 EIO |

**绑定方式**：`backend/gluster/gfapi.hpp` 定义一张 47 项函数表（签名取自 GlusterFS 11 的
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
# read_bps = "0"; write_bps = "0"; iops = 0   # 每导出令牌桶（0 = 不限，热重载）
# anon_uid = 65534; anon_gid = 65534           # squash 目标身份
[export.local]
handles   = "auto"                # auto|kernel|fallback（kernel 需 CAP_DAC_READ_SEARCH）
fd_cache  = 4096
readdir_enrich = true
identity  = "check"               # check|strict|setfsuid（需 root）

# GlusterFS（已实现）：path 只是客户端挂载名，树在卷里
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

# CephFS（已实现，06 §6.8）：path 只是客户端挂载名，树在文件系统里
[[export]]
path      = "/cephfs"
backend   = "cephfs"
fsid      = 4
clients   = ["192.168.0.0/24"]
squash    = "none"                # 身份作为每次调用的 UserPerm 交给 libcephfs
[export.cephfs]
conf           = "/etc/ceph/ceph.conf"   # 空 = 库默认搜索
id             = "lightnfs"       # client id（不带 "client."）
keyring        = "/etc/ceph/ceph.client.lightnfs.keyring"
# mon_host     = "mon1,mon2"      # 覆盖 ceph.conf
fs_name        = "cephfs"         # 空 = 集群默认文件系统
subdir         = "/exports/a"     # 文件系统内导出根，默认 "/"
# log_file     = ""               # libcephfs 日志，空 = 库默认
# options      = "client_mount_timeout=30"   # 额外 ceph_conf_set 键值，逗号分隔
# fd_cache     = 1024             # 匿名 IO Fh 缓存 / inode 句柄缓存容量
# readdir_enrich = true           # readdirplus 带属性 + 句柄
# jukebox      = true             # 传输类错误 → JUKEBOX/DELAY（false → EIO）
# native_locks = true             # ceph_ll_setlk 下推（MDS 全局仲裁）

# Lustre（已实现，06 §6.5）：path 是 Lustre 客户端挂载内的目录
[[export]]
path      = "/mnt/lustre/projects"
backend   = "lustre"
fsid      = 3
clients   = ["10.0.0.0/8"]
squash    = "none"
[export.lustre]
mount          = "/mnt/lustre"    # 挂载根，默认自动探测（沿父目录上溯到 st_dev 变化处）
# fd_cache     = 4096             # 数据 fd / O_PATH 解析缓存容量（同 local）
# identity     = "check"          # check | strict | setfsuid（需 root；鉴权交给 MDS）
# readdir_enrich = true
# hsm          = true             # 已释放文件：提交 RESTORE 并回 JUKEBOX/DELAY（false = 内核隐式 restore，阻塞）
# native_locks = true             # v4 字节锁 → OFD fcntl 锁（-o flock 挂载时 MDS 全局仲裁）
```

## 6.8 映射验证：CephFS 后端 ✅ 已实现（2026-09-04）

第四个后端，也是第一个**同时**具备原生 change 计数与原生字节锁的后端——多网关一致性的两个
前提由此首次在同一个后端上具备。

| Backend API | libcephfs 映射（实现） | 备注 |
|-------------|--------------|------|
| Backend::start/stop | `ceph_create(id)` + `ceph_conf_read_file(conf)` + `ceph_conf_set(keyring/mon_host/log_file/options…)` + `ceph_init` + `ceph_select_filesystem(fs_name)` + `ceph_mount(subdir)`，再 `ceph_ll_lookup_root` + `ceph_ll_getattr` 取导出根（offload kHeavy）；stop 先关锁 Fh/Fh 缓存/inode 缓存/根引用再 `ceph_unmount` + `ceph_release` | 每导出一个 `ceph_mount_info`；工厂构造不连接，`start()` 才连；无 ceph.conf 且未显式配置时只告警（mon_host/keyring 可全部来自配置键） |
| ObjId | 标记字节 `5` + 8B inode 号 + 8B snapid（`vinodeno_t`；snapid 取 `stx_dev`，libcephfs 在此字段回报 inode 的 snapid，Ganesha 同法） → kStableHandles | 17B；Ceph 不复用 inode 号，故无需世代号即满足 P2；`CephBackend::vino_from_oid` 是解析边界（fuzz 目标 `objid_cephfs`），ino 0 直接 ESTALE |
| resolve | `ceph_ll_lookup_vino`（MDS lookup_ino）→ `Inode*`，前置分片 LRU inode 句柄缓存（容量 = fd_cache）；再一次 `ceph_ll_getattr` 定类型 | ENOENT/ESTALE/EINVAL → ESTALE；ObjPtr 共享 `InodeRef`（最后一个用户 `ceph_ll_put`） |
| lookup/create/… | `ceph_ll_lookup/ll_create(O_CREAT\|O_EXCL\|O_RDWR，Fh 立即关闭)/ll_mkdir/ll_mknod/ll_symlink/ll_unlink/ll_rmdir/ll_rename/ll_link/ll_readlink/ll_setattr/ll_statfs` | `..` 在导出根钳到根；unlink 先 lookup 取类型与 ObjId（目录 → EISDIR，且丢弃该文件的缓存 Fh）；创建后若 MDS umask 回调/默认 ACL 改了 mode 则补一次 setattr；EXCLUSIVE verifier 存 atime/mtime（同 local 布局） |
| 身份 | 每次调用在 offload 线程构造一个 `UserPerm`（`ceph_userperm_new(uid,gid,groups)`），调用后销毁；libcephfs 在 `client_permissions` 下判定 | **不置 kNativeAccess**：无 `ceph_ll_access`，ACCESS 由 `Object::access` 默认实现按 mode 位在网关侧回答（一次 getattr）；变更与 OPEN 仍由库以调用者身份权威判定 |
| change | `ceph_statx.stx_version`（MDS change attribute，数据/元数据变更都递增、全客户端一致） → **kNativeChange**；v4.2 `change_attr_type = MONOTONIC_INCR` | mask 缺 VERSION（极旧 MDS）时回落 ctime 合成 |
| open/IO | v4 OPEN：`ceph_ll_open` 以调用者 UserPerm → `CephOpenState`（EACCES 即 OPEN 的答案）；匿名 IO：每对象 Fh 缓存（读→写升级、LRU 驱逐、以网关 uid 0 的 UserPerm 打开，门禁 = mode 位 + v3 属主放宽）；`ceph_ll_read/ll_writev`，kDataSync/kFileSync → `ceph_ll_fsync(1/0)`；commit = `ceph_ll_fsync(…, 1)`，失败粘性 poison（§6.2） | 全部 offload；数据路径 kLight，同步/分配/拷贝 kHeavy；所有返回值为负 errno |
| readdir | `ceph_ll_opendir` + `ceph_seekdir(cookie)` + `ceph_readdirplus_r(want=BASIC\|VERSION, AT_STATX_DONT_SYNC, out=NULL)`，d_off 作 cookie；每页一个目录句柄 | 富化：attr 与 oid 都直接来自 statx（ino + stx_dev），**不取 Inode 引用**——每项零额外往返 |
| v4.2 | `ceph_ll_lseek(SEEK_DATA/HOLE)`（Ceph 无 extent 图：文件内 DATA=偏移本身、HOLE=EOF，越界 ENXIO——RFC 7862 最小实现）/ `ceph_ll_fallocate(0)` / `ceph_ll_fallocate(PUNCH_HOLE\|KEEP_SIZE)` → kSparseOps；copy_range = 网关侧 `ll_read/ll_write` 循环（libcephfs 无 copy_file_range）→ kCopyRange | 无 CLONE |
| locks | `CephLockMgr`：每 (文件, lock-owner) 一个 Fh（网关身份 O_RDWR，EACCES 回落 O_RDONLY），`ceph_ll_setlk(fh, flock, owner64, sleep=0)`，owner64 = LockOwnerId 字节的 FNV-1a；冲突 EAGAIN/EWOULDBLOCK；`test()` 用 owner 0 的探测 Fh 走 `ceph_ll_getlk`；`release()` 全量解锁并关 Fh（Ceph 关 Fh 即释放其锁） → **kByteLocks**，`native_locks()` 交给状态层下推 | MDS 全局仲裁：多网关与原生客户端之间互相看见；`native_locks=false` 关闭 |
| jukebox | ENOTCONN/ETIMEDOUT/ENETDOWN/ENETUNREACH/EHOSTUNREACH/EHOSTDOWN → `kJukebox`（v3 JUKEBOX / v4 DELAY）→ **kJukebox**；EBLOCKLISTED（ESHUTDOWN，会话被列入黑名单）→ **硬 EIO** + `blocklisted` 计数（重试无意义，需重启网关） | MDS failover / OSD 重连期间客户端重试而非报错；`jukebox=false` 时回 EIO |
| Backend::takeover（09 §9.7，10 册 D2，2026-09-05） | 先清锁 Fh/Fh 缓存/inode 缓存/根引用，`ceph_unmount` → `ceph_start_reclaim(uuid, CEPH_RECLAIM_RESET)`（0 → `ceph_finish_reclaim`；-ENOENT = 没有旧会话，视为成功；其他错误只告警）→ 成功时 `ceph_set_uuid(uuid)` → `ceph_mount` + 重新取根。uuid = `[export.cephfs] uuid`，空则 `<cluster id>-<fsid>`（各网关相同） | MDS 立即驱逐持同一 uuid 的故障网关会话并释放其 caps/锁；**先 reclaim 后 set_uuid**——库拒绝回收句柄自身已带的 uuid；Standby 从不带 uuid 挂载（会踢掉活动网关），stop()+start() 回到无 uuid 会话；三个符号在函数表里可选，旧 libcephfs 缺失时 `takeover()` 回 ENOTSUP 并告警；重挂失败则导出停用（同未启动）待下次 `start()` |

**绑定方式**：`backend/cephfs/cephapi.hpp` 定义一张 49 项函数表（其中会话回收的 3 项可选）（签名取自 Ceph 20 的
`cephfs/libcephfs.h` / `cephfs/ceph_ll_client.h`，不透明类型按真名在全局命名空间声明；
`ceph_statx` 与 `vinodeno_t` 在真头文件的 include guard 下自带完整定义），`cephapi.cpp`
在 `start()` 时 `dlopen("libcephfs.so.2")` + `dlsym`（libcephfs 无符号版本）填表——二进制
无构建期 Ceph 依赖；`scripts/check_cephapi_abi.sh` 在有头文件的主机上把每个成员与真实
声明做编译期比对，并用 C 编译单元核对 `vinodeno_t`/`ceph_statx` 布局（已对 20.2.0 头文件
通过）。测试用 `tests/cephapi_fake.cpp` 在本地目录上实现同一张表（UserPerm 身份、永不复用的
合成 inode 号、stx_dev=snapid、每次变更递增的 stx_version、按 (inode, owner) 键并随 Fh 关闭
释放的锁表、可注入传输/黑名单错误、带 uuid 的会话表与 `plant_stale_lock` 植入的故障网关残留锁），
`tests/test_cephfs.cpp` 15 个用例由此把整条后端逻辑跑在
ctest 里；真实集群验收 = `scripts/accept_cephfs.sh`（校验启动日志 `native-change=true
native-locks=true`）。

结论（验证后）：接口**零改动**——gluster 时加的 `LockMgr::release()` 与
`BackendFactory::virtual_path` 直接复用。边界：ACCESS 是网关侧 mode 位（POSIX ACL 只在库
的 OPEN/变更判定里生效，ACCESS 可能比 OPEN 更宽松——客户端本就按 OPEN 结果为准）；
copy_range 是网关内存中转（不经 NFS 线，但吃 offload 线程）；被列入黑名单的会话不自愈；
v4 open/deny 状态仍是网关本地（05 §5.6 口径不变）。
