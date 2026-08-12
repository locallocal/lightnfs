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

## 6.6 映射验证：GlusterFS 后端（未来）

| Backend API | libgfapi 映射 | 备注 |
|-------------|--------------|------|
| Backend::start | `glfs_new/glfs_init`（offload） | 每导出一个 glfs_t |
| ObjId | GFID（16B UUID） | `glfs_h_create_from_handle` 正是 handle-based API（Ganesha 同款） |
| resolve | `glfs_h_create_from_handle` → `glfs_object*` | ObjPtr 持有 glfs_object |
| lookup/creat/… | `glfs_h_lookupat/glfs_h_creat/...` 全套 h_* API | 与 Object 接口一一对应 |
| open/IO | `glfs_h_open` → glfd；匿名 IO：`glfs_h_anonymous_open` 或内部 glfd 缓存 | libgfapi 是阻塞库 → 全部 offload；未来可用 glfs_*_async 族改造 |
| readdir | `glfs_opendir + glfs_readdirplus_r`，d_off 作 cookie | DHT 的 d_off 稳定性已被 Ganesha 验证 |
| change | 无原生 → 不置 kNativeChange | 多网关部署受 5.6 限制 |
| locks | posix-locks xlator 经 glfs_posix_lock → kByteLocks 可置 | — |

结论：接口无缺口；唯一摩擦点是 libgfapi 阻塞调用——offload 池并发度成为该后端吞吐上限，这正是 `offload()` 抽象存在的理由（后端内部可自带专属线程池，接口不变）。

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

# 未来：
# backend = "gluster";  [export.gluster] volfile_server="gs1"; volume="vol0"
# backend = "lustre";   [export.lustre]  mount="/mnt/lustre"
```
