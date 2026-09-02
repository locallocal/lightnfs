#pragma once
// GlusterFS backend over libgfapi (design 06 §6.6, plan doc 10 §5.3 — the second
// backend and the "interface freeze" check of design 09).
//
// Mapping (06 §6.6 table, now real):
//   ObjId            = tag byte + 16-byte GFID (glfs_h_extract_handle) → kStableHandles
//   resolve          = glfs_h_create_from_handle, behind a sharded object-handle cache
//   namespace ops    = glfs_h_* (lookupat/creat/mkdir/mknod/symlink/unlink/rename/link)
//   identity         = glfs_setfsuid/gid/groups on the offload thread per call: the
//                      bricks (posix-acl xlator) authorize → kNativeAccess, access()
//                      answered by glfs_h_access
//   anonymous IO     = per-object glfd cache (read→write upgrade, like the local fd
//                      cache); v4 OPEN gets its own glfd (GlusterOpenState)
//   readdir          = glfs_h_opendir + glfs_xreaddirplus_r(STAT|HANDLE), d_off cookie
//   change           = no native counter → synthesized from ctime (05 §5.6; no
//                      kNativeChange; v4 change_attr_type = TIME_METADATA)
//   v4.2             = glfs_lseek(SEEK_DATA/HOLE) / glfs_fallocate / glfs_discard →
//                      kSparseOps; glfs_copy_file_range → kCopyRange; no CLONE
//   locks            = glfs_posix_lock with glfs_fd_set_lkowner → kByteLocks, exposed
//                      through native_locks() (05 §5.8); the state layer keeps its
//                      gateway table for stateids and additionally pushes every
//                      LOCK/LOCKU into the volume so several gateways stay coherent
//   jukebox          = transport-class errors from the volume (ENOTCONN/ETIMEDOUT/…)
//                      map to kJukebox → v3 JUKEBOX / v4 DELAY: the client retries
//                      instead of failing during brick reconnect / quorum loss
//
// libgfapi is a blocking library: every call runs on the offload pool (02 §2.2); the
// binding is a runtime-loaded function table (backend/gfapi.hpp), so the binary has
// no build-time GlusterFS dependency and the tests drive the same code through an
// in-process fake.

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "backend/api.hpp"
#include "backend/gfapi.hpp"

namespace lnfs::backend {

class GlusterObject;
class GlusterLockMgr;

class GlusterBackend final : public Backend {
 public:
  struct Server {
    std::string host;
    int port = 24007;
  };
  struct Config {
    std::string volume;
    std::string subdir = "/";  // export root inside the volume ("/" = volume root)
    std::vector<Server> servers;  // volfile servers, tried in order; empty = localhost
    std::string transport = "tcp";
    std::string log_file;  // empty: libgfapi's own default (usually unwritable unprivileged)
    int log_level = 4;     // gluster log levels: 0 EMERG … 4 ERROR … 7 INFO … 9 TRACE
    uint64_t fsid = 0;
    size_t fd_cache = 1024;
    bool enrich_readdir = true;
    bool jukebox = true;       // transport-class errors → kJukebox (else EIO)
    bool native_locks = true;  // glfs_posix_lock → kByteLocks / native_locks()
  };

  // `api` null: dlopen the system libgfapi at start().  Construction never touches the
  // volume; start() connects (config load must not block on a cluster).
  static Result<std::unique_ptr<GlusterBackend>> create(
      Config cfg, std::shared_ptr<const gfapi::Api> api = nullptr);
  ~GlusterBackend() override;

  Caps caps() const override { return caps_; }
  FsLimits limits() const override { return limits_; }
  uint64_t fsid() const override { return cfg_.fsid; }
  rt::Task<Result<ObjPtr>> root() override;
  rt::Task<Result<ObjPtr>> resolve(const ObjId&) override;
  rt::Task<Result<FsStats>> statfs() override;
  rt::Task<Result<void>> start() override;
  rt::Task<Result<void>> stop() override;
  std::optional<LockMgrRef> native_locks() override;

  const Config& config() const { return cfg_; }
  bool started() const { return fs_ != nullptr; }
  const std::string& volume_id() const { return volume_id_; }

  struct Stats {
    uint64_t fd_hits = 0, fd_misses = 0, fd_upgrades = 0, fd_evictions = 0;
    size_t fd_entries = 0;
    uint64_t obj_hits = 0, obj_misses = 0;
    size_t obj_entries = 0;
    uint64_t jukebox = 0;  // transport errors surfaced as kJukebox
    size_t lock_fds = 0;   // glfds pinned by native byte-range locks
  };
  Stats stats() const;
  size_t flush_fd_cache();  // `lightnfs-ctl fdcache flush`: drops unpinned entries

  // Sticky fsync failure per design 06 §6.2 (same contract as the local backend).
  void poison(const ObjId& oid);
  bool is_poisoned(const ObjId& oid) const;
  size_t clear_poison();

  // Handle codec (client-controlled bytes → GFID): public for the fuzz target.
  using Gfid = std::array<unsigned char, gfapi::kHandleLength>;
  static Result<Gfid> gfid_from_oid(const ObjId& oid);
  static ObjId oid_from_gfid(const Gfid& gfid);

 private:
  friend class GlusterObject;
  friend class GlusterLockMgr;
  class FdCache;
  class ObjCache;

  // One libgfapi inode handle; closed with glfs_h_close when the last user is gone.
  struct ObjHandle {
    ObjHandle(const gfapi::Api* a, glfs_object* o) : api(a), obj(o) {}
    ~ObjHandle();
    ObjHandle(const ObjHandle&) = delete;
    const gfapi::Api* api;
    glfs_object* obj;
  };
  using ObjRef = std::shared_ptr<ObjHandle>;

  explicit GlusterBackend(Config cfg, std::shared_ptr<const gfapi::Api> api);

  // Offload-thread helpers (blocking libgfapi calls).
  Errno map_errno(int e) const;  // transport-class errors → kJukebox
  Result<Attr> attr_from_stat(const struct stat& st) const;
  Result<Attr> stat_sync(glfs_object* obj) const;
  Result<ObjId> oid_of(glfs_object* obj) const;
  Result<ObjRef> handle_from_oid_sync(const ObjId& oid, struct stat* st);
  ObjPtr wrap(ObjRef ref, const ObjId& oid, FType type);
  Result<ObjPtr> wrap_new(glfs_object* obj, const struct stat& st);  // adopts obj
  static bool valid_name(std::string_view name, bool allow_dotdot = false);

  Config cfg_;
  std::shared_ptr<const gfapi::Api> api_;
  glfs* fs_ = nullptr;
  Caps caps_;
  FsLimits limits_;
  ObjRef root_;
  ObjId root_oid_{};
  std::string volume_id_;
  std::unique_ptr<FdCache> fd_cache_;
  std::unique_ptr<ObjCache> obj_cache_;
  std::unique_ptr<GlusterLockMgr> locks_;
  mutable std::atomic<uint64_t> jukebox_{0};

  mutable std::mutex poison_mu_;
  std::unordered_set<ObjId, ObjIdHash> poisoned_;
};

class GlusterObject final : public Object {
 public:
  rt::Task<Result<Attr>> getattr() override;
  rt::Task<Result<Attr>> setattr(const Cred&, const SetAttr&) override;
  // kNativeAccess: glfs_h_access under the caller's fsuid — the bricks' posix-acl
  // xlator decides (mode bits + POSIX ACLs), the gateway never recomputes.
  rt::Task<Result<AccessMask>> access(const Cred&, AccessMask want) override;
  rt::Task<Result<ObjPtr>> lookup(const Cred&, std::string_view name) override;
  rt::Task<Result<Created>> create(const Cred&, std::string_view, const SetAttr&,
                                    ExclVerf*) override;
  rt::Task<Result<Created>> mkdir(const Cred&, std::string_view, const SetAttr&) override;
  rt::Task<Result<Created>> symlink(const Cred&, std::string_view, std::string_view,
                                     const SetAttr&) override;
  rt::Task<Result<Created>> mknod(const Cred&, std::string_view, FType, DevT,
                                   const SetAttr&) override;
  rt::Task<Result<void>> unlink(const Cred&, std::string_view) override;
  rt::Task<Result<void>> rmdir(const Cred&, std::string_view) override;
  rt::Task<Result<void>> rename(const Cred&, std::string_view, Object&,
                                 std::string_view) override;
  rt::Task<Result<void>> link(const Cred&, Object&, std::string_view) override;
  rt::Task<Result<DirPage>> readdir(const Cred&, uint64_t cookie,
                                     uint32_t max_entries) override;
  rt::Task<Result<std::string>> readlink() override;
  rt::Task<Result<OpenPtr>> open(const Cred&, OpenFlags) override;
  rt::Task<Result<uint32_t>> read(OpenCtx, uint64_t off, std::span<std::byte> out,
                                  bool& eof) override;
  rt::Task<Result<uint32_t>> write(OpenCtx, uint64_t off, std::span<const std::byte> in,
                                   Stability) override;
  rt::Task<Result<uint32_t>> write(OpenCtx, uint64_t off, std::span<const iovec> iov,
                                   Stability) override;
  rt::Task<Result<void>> commit(OpenCtx, uint64_t off, uint64_t len) override;
  rt::Task<Result<uint64_t>> seek(OpenCtx, uint64_t off, SeekWhat) override;
  rt::Task<Result<void>> allocate(OpenCtx, uint64_t off, uint64_t len) override;
  rt::Task<Result<void>> deallocate(OpenCtx, uint64_t off, uint64_t len) override;
  rt::Task<Result<uint64_t>> copy_range(OpenCtx, Object& dst, OpenCtx, uint64_t src_off,
                                        uint64_t dst_off, uint64_t len) override;

  glfs_object* handle() const { return ref_->obj; }

 private:
  friend class GlusterBackend;
  friend class GlusterLockMgr;
  GlusterObject(GlusterBackend& backend, GlusterBackend::ObjRef ref, ObjId id, FType type)
      : Object(std::move(id), type), backend_(backend), ref_(std::move(ref)) {}

  // Anonymous-IO precondition: regular file + native access check for the requested
  // direction (an open state carries its own permission, settled at OPEN time).
  rt::Task<Result<void>> io_gate(const Cred& cred, bool write);
  rt::Task<Result<void>> require_dir(const Cred& cred);
  // Creation-family tail shared by create/mkdir/symlink/mknod (offload thread).
  Result<Created> created_sync(glfs_object* child, const struct stat& st, const Cred& cred,
                               std::optional<uint32_t> want_mode);

  GlusterBackend& backend_;
  GlusterBackend::ObjRef ref_;
};

// Native byte-range locks (design 05 §5.8): one glfd per (file, lock-owner) with
// glfs_fd_set_lkowner so the posix-locks xlator sees distinct owners; the glfd stays
// open while the owner holds anything on the file (release() closes it).  Conflicts
// come back as EAGAIN; test() runs F_GETLK through a probe descriptor.
class GlusterLockMgr final : public LockMgr {
 public:
  explicit GlusterLockMgr(GlusterBackend& backend) : backend_(backend) {}
  ~GlusterLockMgr() override;
  rt::Task<Result<void>> lock(Object&, const LockOwnerId&, LockRange, bool exclusive,
                              bool wait) override;
  rt::Task<Result<void>> unlock(Object&, const LockOwnerId&, LockRange) override;
  rt::Task<Result<std::optional<LockConflict>>> test(Object&, LockRange,
                                                     bool exclusive) override;
  rt::Task<Result<void>> release(Object&, const LockOwnerId&) override;
  size_t fds() const;
  void close_all();  // backend stop

 private:
  struct Key {
    ObjId oid;
    std::string owner;
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  Result<glfs_fd*> fd_for(GlusterObject& obj, const LockOwnerId& owner, bool create);
  static struct flock make_flock(LockRange range, short type);

  GlusterBackend& backend_;
  std::mutex mu_;
  std::unordered_map<Key, glfs_fd*, KeyHash> fds_;
};

// Registers "gluster" with the backend registry (called from register_builtin_backends).
void register_gluster_backend();

}  // namespace lnfs::backend
