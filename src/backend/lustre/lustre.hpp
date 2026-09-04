#pragma once
// Lustre backend (design 06 §6.5 — the third backend; plan doc 10 §5.3 follow-up).
//
// A Lustre client mount is a POSIX tree, so the local backend already covers IO,
// namespace ops, caches and identity; this flavour swaps in what design 06 §6.5
// mapped onto Lustre-specific machinery:
//   ObjId          = tag byte 4 + 16-byte FID (lu_fid: seq/oid/ver) → kStableHandles
//                    without CAP_DAC_READ_SEARCH: FIDs come from the FILEID_LUSTRE
//                    export handle, and resolve opens <mount>/.lustre/fid/<fid>
//                    (llapi_open_by_fid's mechanism) instead of open_by_handle_at
//   IO             = the local backend's io_uring pread/pwrite on the cached data fds
//                    (large stripes stream straight through)
//   change         = ctime-synthesized (05 §5.6): Lustre timestamps are issued by the
//                    MDT, so they are coherent across gateways, but no monotonic
//                    counter reaches userspace (LL_IOC_DATA_VERSION costs an OST
//                    round-trip per stripe and is data-only) — no kNativeChange
//   statfs/limits  = statvfs (Lustre aggregates the OSTs); pref_read/pref_write follow
//                    the export root's default stripe size (lustre.lov)
//   native_locks   = OFD fcntl locks, one descriptor per (file, lock-owner): with the
//                    client mounted `-o flock` the MDS arbitrates them cluster-wide →
//                    kByteLocks, pushed from the v4 state layer (07 册)
//   kJukebox       = HSM: a data-fd open on a RELEASED file kicks LL_IOC_HSM_REQUEST
//                    (RESTORE) and answers kJukebox (v3 JUKEBOX / v4 DELAY) instead of
//                    parking an offload worker on the implicit restore
// The kernel binding is backend/lustre/llapi.hpp (ioctls + .lustre/fid; no liblustreapi);
// tests run the same code on a plain directory through tests/llapi_fake.cpp.

#include <fcntl.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "backend/lustre/llapi.hpp"
#include "backend/local/local.hpp"

namespace lnfs::backend {

class LustreLockMgr;

class LustreBackend final : public LocalBackend {
 public:
  struct Config {
    std::string path;   // export root (a directory inside the Lustre mount)
    uint64_t fsid = 0;
    std::string mount;  // Lustre mount root; empty = walk up from path while st_dev matches
    size_t fd_cache = 4096;
    Identity identity = Identity::kCheck;
    bool enrich_readdir = true;
    bool hsm = true;           // released files → kJukebox + RESTORE kick
    bool native_locks = true;  // OFD locks → kByteLocks / native_locks()
  };

  // `ops` null: the real kernel client.  Fails with EOPNOTSUPP when the mount root is
  // not Lustre (or FIDs cannot be derived), EXDEV when path and mount differ in device.
  static Result<std::unique_ptr<LustreBackend>> create(Config cfg,
                                                       const llapi::Ops* ops = nullptr);
  ~LustreBackend() override;

  rt::Task<Result<void>> stop() override;
  std::optional<LockMgrRef> native_locks() override;

  const Config& lustre_config() const { return lcfg_; }
  const std::string& mount_path() const { return mount_path_; }

  struct Stats {
    uint64_t jukebox = 0;       // data opens answered kJukebox (file released)
    uint64_t hsm_checks = 0;    // HSM state queries (one per regular-file data open)
    uint64_t hsm_restores = 0;  // RESTORE requests submitted
    size_t lock_fds = 0;        // descriptors pinned by native byte-range locks
  };
  Stats stats() const;

  // Handle codec (client-controlled bytes → FID): public for the fuzz target.
  static Result<llapi::Fid> fid_from_oid(const ObjId& oid);
  static ObjId oid_from_fid(const llapi::Fid& fid);

  // FID open (+ HSM gate for data opens); every data-fd site in the local backend
  // reaches Lustre through this override.
  Result<int> open_oid(const ObjId& oid, int flags) override;

 protected:
  Result<ObjId> oid_from_fd(int fd, std::string_view relative, bool remember) override;

 private:
  friend class LustreLockMgr;
  LustreBackend(LocalBackend::Config base, int root_fd, int mount_fd, Config lcfg,
                const llapi::Ops& ops, int lustre_fd, std::string mount_path);
  // Offload thread: released → kick RESTORE once, answer kJukebox.
  Result<void> hsm_gate(int fd, const llapi::Fid& fid);

  Config lcfg_;
  const llapi::Ops& ops_;
  int lustre_fd_ = -1;  // O_RDONLY directory fd on the mount root (.lustre/fid lives there)
  std::string mount_path_;
  std::unique_ptr<LustreLockMgr> locks_;
  std::atomic<uint64_t> jukebox_{0}, hsm_checks_{0}, hsm_restores_{0};
};

// Native byte-range locks over OFD fcntl locks (F_OFD_SETLK/GETLK): one descriptor
// per (file, lock-owner) so distinct NFS owners are distinct kernel lock owners; the
// descriptor stays open while the owner holds anything on the file (release() closes
// it).  Conflicts come back as EAGAIN; test() probes with a fresh descriptor.  On a
// Lustre client mounted `-o flock` these are MDS-arbitrated, i.e. coherent across
// gateways and native clients; with `localflock` (or on any other filesystem) they
// are only process/host-wide.
class LustreLockMgr final : public LockMgr {
 public:
  explicit LustreLockMgr(LustreBackend& backend) : backend_(backend) {}
  ~LustreLockMgr() override;
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
  Result<int> fd_for(const ObjId& oid, const LockOwnerId& owner, bool create);
  static struct flock make_flock(LockRange range, short type);

  LustreBackend& backend_;
  mutable std::mutex mu_;
  std::unordered_map<Key, int, KeyHash> fds_;
};

// Registers "lustre" with the backend registry (called from register_builtin_backends).
void register_lustre_backend();

}  // namespace lnfs::backend
