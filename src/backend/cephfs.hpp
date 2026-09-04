#pragma once
// CephFS backend over libcephfs (design 06 §6.8, plan doc 10 §5.3 — the fourth
// backend; the first with both halves of multi-gateway coherence: a native change
// counter *and* native byte-range locks).
//
// Mapping (06 §6.8 table):
//   ObjId            = tag byte + 8-byte inode number + 8-byte snapid (vinodeno_t; the
//                      snapid is what libcephfs reports in stx_dev) → kStableHandles.
//                      Ceph never reuses inode numbers, so a deleted file's handle
//                      stays ESTALE forever (P2 without a generation number).
//   resolve          = ceph_ll_lookup_vino (MDS lookup_ino), behind a sharded
//                      inode-handle cache; every Inode* is put back with ceph_ll_put
//   namespace ops    = ceph_ll_* (lookup/create/mkdir/mknod/symlink/unlink/rmdir/
//                      rename/link/readlink/setattr)
//   identity         = a UserPerm (uid/gid/groups) per call; libcephfs authorizes
//                      under it (client_permissions) — no kNativeAccess: there is no
//                      ceph_ll_access, ACCESS is answered from mode bits gateway-side
//                      (Object::access), the v4 OPEN is authoritative through
//                      ceph_ll_open under the caller
//   change           = ceph_statx.stx_version (the MDS change attribute) →
//                      kNativeChange; v4.2 change_attr_type = MONOTONIC_INCR
//   anonymous IO     = per-object Fh cache (read→write upgrade, opened as the gateway);
//                      v4 OPEN gets its own Fh (CephOpenState)
//   readdir          = ceph_ll_opendir + ceph_seekdir(cookie) + ceph_readdirplus_r;
//                      d_off cookie, the statx's ino/snapid give the entry's ObjId
//                      without taking an Inode reference
//   v4.2             = ceph_ll_lseek(SEEK_DATA/HOLE) / ceph_ll_fallocate(0) /
//                      ceph_ll_fallocate(PUNCH_HOLE|KEEP_SIZE) → kSparseOps;
//                      copy_range = ll_read/ll_write loop on the gateway (no
//                      copy_file_range in libcephfs) → kCopyRange; no CLONE
//   locks            = ceph_ll_setlk/getlk with a 64-bit owner (MDS-arbitrated fcntl
//                      locks) → kByteLocks via native_locks() (05 §5.8)
//   jukebox          = transport-class errors (ENOTCONN/ETIMEDOUT/ENET*/EHOST*) →
//                      kJukebox (v3 JUKEBOX / v4 DELAY); a blocklisted session
//                      (EBLOCKLISTED = ESHUTDOWN) is permanent → EIO + counter
//
// libcephfs is a blocking library: every call runs on the offload pool (02 §2.2);
// the binding is a runtime-loaded function table (backend/cephapi.hpp), so the
// binary has no build-time Ceph dependency and the tests drive the same code through
// an in-process fake.

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backend/api.hpp"
#include "backend/cephapi.hpp"

namespace lnfs::backend {

class CephObject;
class CephLockMgr;

class CephBackend final : public Backend {
 public:
  struct Config {
    std::string conf;      // ceph.conf; empty = library defaults ($CEPH_CONF, /etc/ceph/ceph.conf)
    std::string id;        // client id without the "client." prefix; empty = library default
    std::string keyring;   // ceph_conf_set("keyring", …)
    std::string mon_host;  // ceph_conf_set("mon_host", …)
    std::string fs_name;   // ceph_select_filesystem; empty = the cluster's default fs
    std::string subdir = "/";  // export root inside the filesystem (the mount root)
    std::string log_file;      // ceph_conf_set("log_file", …); empty = library default
    std::vector<std::pair<std::string, std::string>> options;  // extra ceph_conf_set pairs
    uint64_t fsid = 0;
    size_t fd_cache = 1024;
    bool enrich_readdir = true;
    bool jukebox = true;       // transport-class errors → kJukebox (else EIO)
    bool native_locks = true;  // ceph_ll_setlk → kByteLocks / native_locks()
  };

  // `api` null: dlopen the system libcephfs at start().  Construction never touches
  // the cluster; start() connects (config load must not block on a cluster).
  static Result<std::unique_ptr<CephBackend>> create(
      Config cfg, std::shared_ptr<const cephapi::Api> api = nullptr);
  ~CephBackend() override;

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
  bool started() const { return mount_ != nullptr; }
  const std::string& cluster_fsid() const { return cluster_fsid_; }
  int64_t fscid() const { return fscid_; }

  struct Stats {
    uint64_t fd_hits = 0, fd_misses = 0, fd_upgrades = 0, fd_evictions = 0;
    size_t fd_entries = 0;
    uint64_t obj_hits = 0, obj_misses = 0;
    size_t obj_entries = 0;
    uint64_t jukebox = 0;      // transport errors surfaced as kJukebox
    uint64_t blocklisted = 0;  // EBLOCKLISTED seen (permanent until restart)
    size_t lock_fds = 0;       // Fh pinned by native byte-range locks
  };
  Stats stats() const;
  size_t flush_fd_cache();  // `lightnfs-ctl fdcache flush`: drops unpinned entries

  // Sticky fsync failure per design 06 §6.2 (same contract as the local backend).
  void poison(const ObjId& oid);
  bool is_poisoned(const ObjId& oid) const;
  size_t clear_poison();

  // Handle codec (client-controlled bytes → vinodeno): public for the fuzz target.
  static Result<vinodeno_t> vino_from_oid(const ObjId& oid);
  static ObjId oid_from_vino(const vinodeno_t& vino);

 private:
  friend class CephObject;
  friend class CephLockMgr;
  class FdCache;
  class ObjCache;

  // One libcephfs inode reference; put back when the last user is gone.
  struct InodeRef {
    InodeRef(const cephapi::Api* a, ceph_mount_info* m, Inode* i) : api(a), mount(m), in(i) {}
    ~InodeRef();
    InodeRef(const InodeRef&) = delete;
    const cephapi::Api* api;
    ceph_mount_info* mount;
    Inode* in;
  };
  using ObjRef = std::shared_ptr<InodeRef>;

  explicit CephBackend(Config cfg, std::shared_ptr<const cephapi::Api> api);

  // Offload-thread helpers (blocking libcephfs calls).  `rc` is a libcephfs return
  // value (negative errno).
  Errno map_rc(int64_t rc) const;
  Result<Attr> attr_from_statx(const struct ceph_statx& st) const;
  Result<Attr> stat_sync(Inode* in, const UserPerm* perms) const;
  static ObjId oid_of(const struct ceph_statx& st);
  ObjPtr wrap(ObjRef ref, const ObjId& oid, FType type);
  Result<ObjPtr> wrap_new(Inode* in, const struct ceph_statx& st);  // adopts the reference
  static bool valid_name(std::string_view name, bool allow_dotdot = false);

  Config cfg_;
  std::shared_ptr<const cephapi::Api> api_;
  ceph_mount_info* mount_ = nullptr;
  UserPerm* root_perms_ = nullptr;  // the gateway's own identity (fd cache, locks)
  Caps caps_;
  FsLimits limits_;
  ObjRef root_;
  ObjId root_oid_{};
  std::string cluster_fsid_;
  int64_t fscid_ = -1;
  std::unique_ptr<FdCache> fd_cache_;
  std::unique_ptr<ObjCache> obj_cache_;
  std::unique_ptr<CephLockMgr> locks_;
  mutable std::atomic<uint64_t> jukebox_{0};
  mutable std::atomic<uint64_t> blocklisted_{0};

  mutable std::mutex poison_mu_;
  std::unordered_set<ObjId, ObjIdHash> poisoned_;
};

class CephObject final : public Object {
 public:
  rt::Task<Result<Attr>> getattr() override;
  rt::Task<Result<Attr>> setattr(const Cred&, const SetAttr&) override;
  // access(): the Object default (mode bits from getattr) — libcephfs has no access
  // call; mutations and opens are still authorized by the library under the caller.
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

  Inode* handle() const { return ref_->in; }

 private:
  friend class CephBackend;
  friend class CephLockMgr;
  CephObject(CephBackend& backend, CephBackend::ObjRef ref, ObjId id, FType type)
      : Object(std::move(id), type), backend_(backend), ref_(std::move(ref)) {}

  // Anonymous-IO precondition: regular file + mode-bit check for the requested
  // direction (an open state carries its own permission, settled at OPEN time).
  rt::Task<Result<void>> io_gate(const Cred& cred, bool write);
  rt::Task<Result<void>> require_dir(const Cred& cred);
  // Creation-family tail shared by create/mkdir/symlink/mknod (offload thread).
  Result<Created> created_sync(Inode* child, struct ceph_statx st, const UserPerm* perms,
                               std::optional<uint32_t> want_mode);

  CephBackend& backend_;
  CephBackend::ObjRef ref_;
};

// Native byte-range locks (design 05 §5.8): one Fh per (file, lock-owner) — Ceph
// keys fcntl locks by (session, owner, pid) and drops what an Fh holds when it is
// closed, so the Fh stays open while the owner holds anything on the file
// (release() closes it).  The 64-bit owner handed to libcephfs is a hash of the
// gateway's LockOwnerId bytes.  Conflicts come back as EAGAIN; test() runs
// ceph_ll_getlk through a probe Fh under an owner no client can have.
class CephLockMgr final : public LockMgr {
 public:
  explicit CephLockMgr(CephBackend& backend) : backend_(backend) {}
  ~CephLockMgr() override;
  rt::Task<Result<void>> lock(Object&, const LockOwnerId&, LockRange, bool exclusive,
                              bool wait) override;
  rt::Task<Result<void>> unlock(Object&, const LockOwnerId&, LockRange) override;
  rt::Task<Result<std::optional<LockConflict>>> test(Object&, LockRange,
                                                     bool exclusive) override;
  rt::Task<Result<void>> release(Object&, const LockOwnerId&) override;
  size_t fds() const;
  void close_all();  // backend stop

  static uint64_t owner_key(const LockOwnerId& owner);  // FNV-1a over the bytes

 private:
  struct Key {
    ObjId oid;
    std::string owner;
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  Result<Fh*> fh_for(CephObject& obj, const LockOwnerId& owner, bool create);
  static struct flock make_flock(LockRange range, short type);

  CephBackend& backend_;
  std::mutex mu_;
  std::unordered_map<Key, Fh*, KeyHash> fhs_;
};

// Registers "cephfs" with the backend registry (called from register_builtin_backends).
void register_cephfs_backend();

}  // namespace lnfs::backend
