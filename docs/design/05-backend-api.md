# 5. 后端抽象接口（Backend API）—— 核心交付物

本接口是 lightnfs 的南向边界：协议层（core 及以上）**只**通过它访问存储。设计目标：

1. **协议无感**：接口只讲文件系统语言（对象、属性、字节、目录游标、errno），不出现 nfsstat/stateid/COMPOUND 等字样；
2. **一次定型**：以本地 POSIX（v1）、Lustre、GlusterFS（libgfapi）三个后端做映射验证（见 [06-backends.md](06-backends.md)），避免 v2 破坏性改版；
3. **全异步**：所有可能等待的方法返回 `Task<Result<T>>`；
4. **能力协商**：后端声明能力位，core/引擎据此裁剪协议行为（NOTSUPP/属性位/FSINFO 通告），而非在后端里造假。

参考系：NFS-Ganesha 的 FSAL 是同类接口的既有实践，本设计吸收其"handle-based 对象模型"，但砍掉其巨型 ops 表 + 裸指针风格，用 C++20 协程与强类型重做。

## 5.1 基础类型

```cpp
namespace lnfs::backend {

using rt::Task;
template <class T> using Result = tl::expected<T, Errno>;   // Errno: 强类型 POSIX errno + kJukebox

// ── 持久对象标识 ─────────────────────────────────────────────
// 后端自定义编码，进入文件句柄（04 分册 4.3），必须满足：
//  P1 同一对象跨进程/跨节点重启恒定不变
//  P2 不同对象（含删除后重建的同名对象）必须不同
//  P3 长度 ≤ kMaxObjId（51B）
struct ObjId {
    static constexpr size_t kMax = 51;
    uint8_t len;  std::array<std::byte, kMax> bytes;
    friend auto operator<=>(const ObjId&, const ObjId&) = default;
};

// ── 属性 ────────────────────────────────────────────────────
enum class FType : uint8_t { kReg=1, kDir, kBlk, kChr, kLnk, kSock, kFifo };

struct Attr {                       // ≈ statx 子集 + change
    FType     type;
    uint32_t  mode;                 // 12 位权限
    uint32_t  nlink;
    uint32_t  uid, gid;
    uint64_t  size, used;
    DevT      rdev;
    uint64_t  fileid;               // fs 内唯一（fattr3.fileid / v4 fileid）
    Timespec  atime, mtime, ctime;
    uint64_t  change;               // 单调变化计数（见 5.6）
};

struct SetAttr {                    // 全字段可选（≈ sattr3 / v4 SETATTR）
    std::optional<uint32_t> mode, uid, gid;
    std::optional<uint64_t> size;                    // truncate/extend
    enum class TimeHow { kOmit, kServer, kClient };
    TimeHow atime_how = TimeHow::kOmit;  Timespec atime;
    TimeHow mtime_how = TimeHow::kOmit;  Timespec mtime;
};

struct Cred { uint32_t uid, gid; std::span<const uint32_t> gids; };
// 每次调用显式传 Cred：后端负责以该身份执行（本地后端 = 权限位自查 + 可选
// setfsuid 路径；gluster/lustre = 透传给存储侧鉴权）。squash 已在上层完成。

struct FsStats  { uint64_t tbytes, fbytes, abytes, tfiles, ffiles, afiles; };
struct FsLimits { uint32_t max_read, max_write, pref_read, pref_write, pref_readdir;
                  uint64_t max_filesize; uint32_t max_name, max_link; Timespec time_delta; };
```

## 5.2 能力位

```cpp
enum class Cap : uint64_t {
    kSymlink        = 1 << 0,   // → v3 SYMLINK/FSF3_SYMLINK、v4 symlink_support
    kHardlink       = 1 << 1,   // → LINK/FSF3_LINK、v4 link_support
    kMknod          = 1 << 2,   // → MKNOD（不支持 → NOTSUPP）
    kNativeAccess   = 1 << 3,   // 后端能权威回答 access()（服务端 ACL 场景）
    kNativeChange   = 1 << 4,   // change 来自存储原生计数（否则 core 按 5.6 降级合成）
    kStableHandles  = 1 << 5,   // ObjId 满足 P1（跨重启持久）—— v1 后端必须置位
    kSparseOps      = 1 << 6,   // seek_hole/allocate/deallocate → v4.2 三件套
    kCloneRange     = 1 << 7,   // clone() → v4.2 CLONE
    kCopyRange      = 1 << 8,   // copy_range() → v4.2 COPY（同服）
    kByteLocks      = 1 << 9,   // 后端下沉字节锁（多网关一致性需要，见 5.8）
    kCaseInsensitive= 1 << 10,  // → PATHCONF/v4 case_insensitive
    kJukebox        = 1 << 11,  // 可能返回 kJukebox（HSM 类后端）
};
using Caps = Flags<Cap>;
```

引擎消费规则示例：无 kSparseOps → v4.2 SEEK/ALLOCATE/DEALLOCATE 回 NOTSUPP；无 kSymlink → v3 SYMLINK 回 NOTSUPP 且 FSINFO properties 不置位——**协议行为随能力位自动收缩，后端永远不需要"假装支持"**。

## 5.3 对象接口

```cpp
class Object;  using ObjPtr = std::shared_ptr<Object>;

class Object {                       // 活对象：后端资源的 RAII 载体
public:
    virtual ~Object();
    const ObjId& id() const;         // 满足 P1–P3
    FType        type() const;       // 构造时即知，无 IO

    // ── 元数据 ──
    virtual Task<Result<Attr>> getattr() = 0;
    virtual Task<Result<Attr>> setattr(const Cred&, const SetAttr&) = 0;   // 返回新属性
    virtual Task<Result<AccessMask>> access(const Cred&, AccessMask want); // 默认实现：getattr+权限位计算；kNativeAccess 后端覆写

    // ── 目录 ──
    virtual Task<Result<ObjPtr>>  lookup(const Cred&, std::string_view name) = 0;
    virtual Task<Result<Created>> create (const Cred&, std::string_view, const SetAttr&, ExclVerf* = nullptr) = 0; // 常规文件
    virtual Task<Result<Created>> mkdir  (const Cred&, std::string_view, const SetAttr&) = 0;
    virtual Task<Result<Created>> symlink(const Cred&, std::string_view, std::string_view target, const SetAttr&) = 0;
    virtual Task<Result<Created>> mknod  (const Cred&, std::string_view, FType, DevT, const SetAttr&) = 0;
    virtual Task<Result<void>>    unlink (const Cred&, std::string_view name) = 0;   // 文件
    virtual Task<Result<void>>    rmdir  (const Cred&, std::string_view name) = 0;   // 目录（非空→ENOTEMPTY）
    virtual Task<Result<void>>    rename (const Cred&, std::string_view from, Object& dst_dir, std::string_view to) = 0;
    virtual Task<Result<void>>    link   (const Cred&, Object& file, std::string_view name) = 0;
    virtual Task<Result<DirPage>> readdir(const Cred&, uint64_t cookie, uint32_t max_entries) = 0;  // 契约见 5.7

    // ── 符号链接 ──
    virtual Task<Result<std::string>> readlink() = 0;

    // ── 常规文件 IO ──
    virtual Task<Result<OpenPtr>> open(const Cred&, OpenFlags) = 0;        // 见 5.5
    virtual Task<Result<uint32_t>> read (OpenCtx, uint64_t off, std::span<std::byte> out, bool& eof) = 0;
    virtual Task<Result<uint32_t>> write(OpenCtx, uint64_t off, std::span<const std::byte> in, Stability) = 0;
    virtual Task<Result<void>>     commit(OpenCtx, uint64_t off, uint64_t len) = 0;  // len=0 → 到 EOF

    // ── 可选扩展（默认返回 ENOTSUP；置位对应 Cap 才覆写）──
    virtual Task<Result<uint64_t>> seek (OpenCtx, uint64_t off, SeekWhat);           // kSparseOps
    virtual Task<Result<void>>     allocate  (OpenCtx, uint64_t off, uint64_t len);  // kSparseOps
    virtual Task<Result<void>>     deallocate(OpenCtx, uint64_t off, uint64_t len);  // kSparseOps
    virtual Task<Result<void>>     clone(OpenCtx src, Object& dst, OpenCtx, uint64_t soff, uint64_t doff, uint64_t len); // kCloneRange
    virtual Task<Result<uint64_t>> copy_range(OpenCtx src, Object& dst, OpenCtx, uint64_t soff, uint64_t doff, uint64_t len); // kCopyRange
};

struct Created { ObjPtr obj; Attr attr; };   // 创建类操作一次返回对象+属性（省一次 getattr）
struct DirPage {
    struct Ent { std::string name; uint64_t cookie; uint64_t fileid;
                 std::optional<Attr> attr; std::optional<ObjId> oid; };  // attr/oid 尽力提供（READDIRPLUS 免逐项 lookup）
    SmallVec<Ent, 64> ents;  bool eof;
};
```

## 5.4 文件系统级接口与注册

```cpp
class Backend {                       // 一个导出 = 一个 Backend 实例
public:
    virtual ~Backend();
    virtual Caps      caps()   const = 0;
    virtual FsLimits  limits() const = 0;
    virtual uint64_t  fsid()   const = 0;              // 稳定的文件系统标识

    virtual Task<Result<ObjPtr>>  root() = 0;
    virtual Task<Result<ObjPtr>>  resolve(const ObjId&) = 0;   // 句柄→对象；不存在→ESTALE
    virtual Task<Result<FsStats>> statfs() = 0;

    virtual Task<Result<void>> start();                // 挂载/连接（gluster: glfs_init）
    virtual Task<Result<void>> stop();
    virtual std::optional<LockMgrRef> native_locks();  // kByteLocks 时提供（5.8）
};

// 工厂注册：配置驱动实例化
struct BackendFactory {
    std::string_view name;                              // "local" | "lustre" | "gluster"
    std::unique_ptr<Backend> (*make)(const toml::table& export_cfg);
};
void register_backend(BackendFactory);                  // 静态注册宏 LNFS_REGISTER_BACKEND(...)
```

## 5.5 打开状态：OpenPtr / OpenCtx

v3 无 open、v4 有 open，接口用"**可选的打开上下文**"统一两者：

```cpp
class OpenState { /* 后端私有资源：fd、glfd、条带句柄… */ };
using OpenPtr = std::shared_ptr<OpenState>;

struct OpenCtx {                       // IO 调用的第一参数
    const Cred&      cred;
    OpenState*       open = nullptr;   // v4：来自 OPEN 的 OpenPtr；v3：nullptr（匿名 IO）
};
```

契约：

- `open == nullptr`（v3 路径 / v4 特殊 stateid）时后端**必须**仍能完成 IO——本地后端用内部 fd 缓存按需开（06 分册 6.3），gluster 用匿名 fd（`glfs_h_anonymous_open`语义）。

**实现（2026-08-28，plan doc 10 §5.1）**：`LocalObject::open` 已实现——每 OPEN 一个
独立数据 fd（读 O_RDONLY / 写 O_RDWR），read/write/seek 优先走它（open 时定权限的
POSIX 语义，免 fd 缓存往返）；打不开时降级返回 EOPNOTSUPP，引擎继续走匿名路径，
行为与之前一致。同 owner 合并升级（读→读写）时状态层保留原句柄，写路径检测到句柄
不可写自动回落 fd 缓存。memory 后端维持 EOPNOTSUPP。**第二个生产者**（2026-09-03，
plan doc 10 §5.3）：gluster 的 `GlusterOpenState` 持每 OPEN 一个 glfd（`glfs_h_open`
以调用者身份打开）；kNativeAccess 后端的 `open()` 不做 EOPNOTSUPP 降级——EACCES 就是
OPEN 的答案，v4 引擎据此跳过网关侧 `access()` 预检。
- v4 引擎把 OPEN→`open()` 的 OpenPtr 存入状态表，CLOSE 时释放（shared_ptr 归零 → 后端资源回收）；同一文件多次 OPEN 合并由状态层负责，后端只见 open/close 配对。
- `OpenFlags`：read/write/create(3 模式)/truncate；EXCLUSIVE 创建的 verifier 由 `create(..., ExclVerf*)` 传入，后端负责持久化到 atime/mtime 并在重放时比对（语义 nfsv3/04 §8——两版协议共用此实现）。

## 5.6 change 属性契约

- kNativeChange 后端：`Attr::change` 来自存储原生计数（statx `STATX_CHANGE_COOKIE`、CephFS `stx_version`；Lustre/gluster 无 → 不置位）。
- 无此能力时 **core 合成**：`change = ctime.sec*1e9 + ctime.nsec`，并在网关内对活跃对象维护"最近一次本网关修改后的单调递增修正"（防同 ns 双改）。多网关同挂一个后端时合成 change 不可靠——文档级限制，多网关部署要求 kNativeChange 后端。
- **消费端（2026-09-03）**：v4.2 属性 `change_attr_type`(79) 据此位宣告 MONOTONIC_INCR
  （原生计数）或 TIME_METADATA（ctime 合成）；`core::FsProps::native_change` 是引擎侧
  的读取点。gluster/lustre 后端不置位（无原生计数）；cephfs 后端置位（MDS change attribute，2026-09-04）。

## 5.7 readdir cookie 契约（后端必须满足）

- cookie 是**目录内位置的稳定标记**：目录发生增删后，旧 cookie 继续读**不重复、不遗漏未变项**（nfsv3 分册 7.6 的弱保证）；
- 取值域 ≥3（0/1/2 归 core 的 `.`/`..` 合成，见 04 分册 4.7；后端若原生 cookie 可能 <3，由 core 做偏移，后端无感知）；
- 本地后端用 getdents64 的 d_off 天然满足；无法满足稳定性的后端必须自建索引（宁可后端复杂，不把 BAD_COOKIE 风暴丢给客户端）。

## 5.8 字节锁下沉接口（v1 不实现，占位定型）

```cpp
class LockMgr {                        // 后端原生锁（多网关共享后端时需要）
public:
    virtual Task<Result<void>> lock  (Object&, const LockOwnerId&, LockRange, bool exclusive, bool wait) = 0;
    virtual Task<Result<void>> unlock(Object&, const LockOwnerId&, LockRange) = 0;
    virtual Task<Result<std::optional<LockConflict>>> test(Object&, LockRange, bool exclusive) = 0;
};
```

v1：网关内 LockMgr（07 分册）实现同一接口，单网关正确；`native_locks()` 有值时状态层改用后端实现（Lustre flock 语义、gluster posix-locks xlator）。接口先行，切换点唯一。

**实现（2026-09-03，plan doc 10 §5.3）**：接口新增一个可选操作
`release(Object&, const LockOwnerId&)`（默认 = 全区间 `unlock`；gluster 覆写为关闭该
owner 的 glfd）。状态层不是"改用"而是"叠加"：网关表继续负责 stateid/本地冲突/courtesy
回收/CB_NOTIFY_LOCK，`StateMgr::Config::native_locks` 钩子把每次 LOCK/LOCKU/LOCKT
额外下推——后端拒绝（EAGAIN）则回滚网关授予并回 DENIED，后端错误回 DELAY/SERVERFAULT。
第一个真实实现是 `GlusterLockMgr`（`glfs_posix_lock` + `glfs_fd_set_lkowner`）；其后
`LustreLockMgr`（OFD 锁）与 `CephLockMgr`（`ceph_ll_setlk`，MDS 仲裁）复用同一条下推链路。

## 5.9 语义契约汇总（后端实现者检查表）

| 方法 | 必须保证 | 允许 |
|------|----------|------|
| resolve | P1/P2：删除后重建的对象返回新 ObjId；不存在 → ESTALE | 内部缓存 ObjPtr |
| create/mkdir/… | 目录项操作原子；EEXIST/ENOTEMPTY 语义准确 | 忽略 SetAttr 中不支持字段（返回实际生效属性） |
| rename | 同后端内原子；跨 Backend 由 core 提前拦截（EXDEV） | — |
| write(kFileSync) | 返回前数据+元数据落稳定存储 | 提升稳定级（Unstable 请求按 FileSync 做） |
| commit | 返回后区间数据可跨崩溃存活 | 刷得更多 |
| readdir | 5.7 契约 | eof 后多余 cookie 返回空页+eof |
| getattr | mtime/ctime/size 即时可见（**不得缓存过期值**——CTO 依赖，nfsv3/07 红线） | — |
| 所有方法 | 线程安全（core 的 objlock 只保证采样原子性，不替后端挡并发）；errno 语义按 POSIX | 返回 kJukebox（置 kJukebox 位时） |

## 5.10 接口演进规则

- 新增可选操作：基类给 ENOTSUP 默认实现 + 新 Cap 位——二进制兼容。
- 语义变更/必选操作增加：bump `kBackendApiVersion`（工厂注册时校验），不做隐式兼容。
- 严禁在接口中出现协议类型；如果某协议特性表达不了（如未来委托下沉），先在本文件加"映射验证"章节论证再动接口。
