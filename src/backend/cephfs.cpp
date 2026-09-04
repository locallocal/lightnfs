#include "backend/cephfs.hpp"

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <ctime>
#include <limits>

#include "backend/fault.hpp"
#include "runtime/io.hpp"
#include "runtime/offload_pool.hpp"
#include "util/log.hpp"
#include "util/small_vec.hpp"

namespace lnfs::backend {
namespace {

constexpr std::byte kCephHandle{5};  // ObjId tag (1/2 local, 3 gluster, 4 lustre)
constexpr size_t kObjIdLen = 1 + 8 + 8;  // tag + ino + snapid

// Attributes every statx asks for: the basic set plus the change attribute.
constexpr unsigned kWant = cephapi::kStatxBasicStats | cephapi::kStatxVersion;

#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif
constexpr int kBlocklisted = ESHUTDOWN;  // libcephfs EBLOCKLISTED

FType mode_type(mode_t mode) {
  if (S_ISREG(mode)) return FType::kReg;
  if (S_ISDIR(mode)) return FType::kDir;
  if (S_ISBLK(mode)) return FType::kBlk;
  if (S_ISCHR(mode)) return FType::kChr;
  if (S_ISLNK(mode)) return FType::kLnk;
  if (S_ISSOCK(mode)) return FType::kSock;
  return FType::kFifo;
}

Timespec convert_time(const timespec& ts) {
  return Timespec{ts.tv_sec, static_cast<uint32_t>(ts.tv_nsec)};
}

timespec to_timespec(const Timespec& t) {
  return timespec{static_cast<time_t>(t.sec), static_cast<long>(t.nsec)};
}

// A Cred whose group list survives a hop to the offload pool.
struct OwnedCred {
  uint32_t uid, gid;
  SmallVec<uint32_t, 32> gids;
  explicit OwnedCred(const Cred& c) : uid(c.uid), gid(c.gid) {
    for (uint32_t g : c.gids) gids.push_back(g);
  }
};

// Caller identity for the duration of one libcephfs call (design 06 §6.8 "透传给
// 存储侧鉴权"): a UserPerm built on the offload worker right before the call and
// destroyed after it.  libcephfs copies the group list.
class Perms {
 public:
  Perms(const cephapi::Api& api, const OwnedCred& cred) : api_(api) {
    SmallVec<gid_t, 32> groups;
    for (uint32_t g : cred.gids) groups.push_back(g);
    perm_ = api_.ceph_userperm_new(cred.uid, cred.gid, static_cast<int>(groups.size()),
                                   groups.size() ? groups.data() : nullptr);
  }
  ~Perms() {
    if (perm_) api_.ceph_userperm_destroy(perm_);
  }
  Perms(const Perms&) = delete;
  const UserPerm* get() const { return perm_; }

 private:
  const cephapi::Api& api_;
  UserPerm* perm_ = nullptr;
};

// EXCLUSIVE create verifier split (nfsv3/04 §8; same layout as the local backend so
// the verifier survives a migration between backends of the same tree).
void verf_split(const ExclVerf& verf, int64_t& lo, int64_t& hi) {
  uint32_t l = 0, h = 0;
  std::memcpy(&l, verf.data(), 4);
  std::memcpy(&h, verf.data() + 4, 4);
  lo = l;
  hi = h;
}

}  // namespace

// ---- inode-handle cache ---------------------------------------------------------

// ObjId → live Inode reference (plan doc 10 §2.1 analogue of the O_PATH resolve
// cache): resolve() is hit at the top of every request; a hit costs a shard mutex
// instead of a lookup_ino round trip to the MDS.  Entries pin the inode in the
// client's cache until eviction — the same bounded-staleness trade as local.
class CephBackend::ObjCache {
 public:
  struct Entry {
    Entry(const ObjId& id, ObjRef r, FType t) : oid(id), ref(std::move(r)), type(t) {}
    ObjId oid;
    ObjRef ref;
    FType type;
    Entry* prev = nullptr;
    Entry* next = nullptr;
  };
  using EntryRef = std::shared_ptr<Entry>;

  explicit ObjCache(size_t capacity)
      : per_shard_capacity_(std::max<size_t>((capacity + kShards - 1) / kShards, 1)) {}

  EntryRef find(const ObjId& oid) {
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    std::lock_guard lock(shard.mu);
    auto it = shard.entries.find(oid);
    if (it == shard.entries.end()) {
      misses_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    touch(shard, it->second.get());
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }

  EntryRef insert(const ObjId& oid, EntryRef value) {
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    std::lock_guard lock(shard.mu);
    auto [it, inserted] = shard.entries.emplace(oid, value);
    if (inserted) push_back(shard, value.get());
    else {
      touch(shard, it->second.get());
      value = it->second;
    }
    size_t budget = shard.entries.size();
    while (shard.entries.size() > per_shard_capacity_ && budget-- > 0) {
      Entry* victim = shard.lru_head;
      auto vit = shard.entries.find(victim->oid);
      if (vit->second.use_count() == 1) {
        unlink(shard, victim);
        shard.entries.erase(vit);
      } else {
        touch(shard, victim);
      }
    }
    return value;
  }

  size_t flush() {
    size_t dropped = 0;
    for (auto& shard : shards_) {
      std::lock_guard lock(shard.mu);
      for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (it->second.use_count() == 1) {
          unlink(shard, it->second.get());
          it = shard.entries.erase(it);
          ++dropped;
        } else {
          ++it;
        }
      }
    }
    return dropped;
  }

  void clear() {
    for (auto& shard : shards_) {
      std::lock_guard lock(shard.mu);
      shard.entries.clear();
      shard.lru_head = shard.lru_tail = nullptr;
    }
  }

  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  size_t entries() const {
    size_t n = 0;
    for (const auto& shard : shards_) {
      std::lock_guard lock(const_cast<std::mutex&>(shard.mu));
      n += shard.entries.size();
    }
    return n;
  }

 private:
  static constexpr size_t kShards = 16;
  struct Shard {
    std::mutex mu;
    std::unordered_map<ObjId, EntryRef, ObjIdHash> entries;
    Entry* lru_head = nullptr;
    Entry* lru_tail = nullptr;
  };
  static void push_back(Shard& shard, Entry* e) {
    e->prev = shard.lru_tail;
    e->next = nullptr;
    (shard.lru_tail ? shard.lru_tail->next : shard.lru_head) = e;
    shard.lru_tail = e;
  }
  static void unlink(Shard& shard, Entry* e) {
    (e->prev ? e->prev->next : shard.lru_head) = e->next;
    (e->next ? e->next->prev : shard.lru_tail) = e->prev;
    e->prev = e->next = nullptr;
  }
  static void touch(Shard& shard, Entry* e) {
    if (shard.lru_tail == e) return;
    unlink(shard, e);
    push_back(shard, e);
  }

  size_t per_shard_capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0}, misses_{0};
};

// ---- anonymous-IO Fh cache ---------------------------------------------------------

// Same shape as the local backend's FdCache (06 §6.3): one Fh per object, read
// acquires reuse anything, write acquires need O_RDWR and upgrade in place.  The Fh
// is opened as the gateway itself (uid 0), not as the requester: the per-IO gate
// (mode bits + the v3 owner relaxation) has already decided, and a shared handle must
// not carry the first requester's identity to later ones.  The v4 OPEN path
// (CephObject::open) is the one that opens under the caller.
class CephBackend::FdCache {
 public:
  struct Entry {
    Entry(const cephapi::Api* a, ceph_mount_info* m, const ObjId& id, Fh* value, int mode)
        : api(a), mount(m), oid(id), fh(value), accmode(mode) {}
    ~Entry() {
      if (fh) api->ceph_ll_close(mount, fh);
    }
    const cephapi::Api* api;
    ceph_mount_info* mount;
    ObjId oid;
    Fh* fh;
    int accmode;
    Entry* prev = nullptr;
    Entry* next = nullptr;
  };
  using Ref = std::shared_ptr<Entry>;

  FdCache(CephBackend& backend, size_t capacity)
      : backend_(backend),
        per_shard_capacity_(std::max<size_t>((capacity + kShards - 1) / kShards, 1)) {}

  rt::Task<Result<Ref>> acquire(CephObject& obj, bool write) {
    const ObjId& oid = obj.id();
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    {
      std::lock_guard lock(shard.mu);
      auto it = shard.entries.find(oid);
      if (it != shard.entries.end() && (!write || it->second->accmode == O_RDWR)) {
        touch(shard, it->second.get());
        hits_.fetch_add(1, std::memory_order_relaxed);
        co_return it->second;
      }
      (write && it != shard.entries.end() ? upgrades_ : misses_)
          .fetch_add(1, std::memory_order_relaxed);
    }
    int flags = write ? O_RDWR : O_RDONLY;
    auto ref = obj.ref_;  // keeps the Inode alive across the hop
    auto opened = co_await rt::offload([this, ref, flags]() -> Result<Fh*> {
      const auto& api = *backend_.api_;
      Fh* fh = nullptr;
      int rc = api.ceph_ll_open(backend_.mount_, ref->in, flags, &fh, backend_.root_perms_);
      if (rc < 0) return Err(backend_.map_rc(rc));
      return fh;
    });
    if (!opened) co_return Err(opened.error());
    auto value =
        std::make_shared<Entry>(backend_.api_.get(), backend_.mount_, oid, *opened, flags);
    {
      std::lock_guard lock(shard.mu);
      auto [it, inserted] = shard.entries.emplace(oid, value);
      if (inserted) {
        push_back(shard, value.get());
      } else if (write && it->second->accmode != O_RDWR) {
        unlink(shard, it->second.get());
        it->second = value;
        push_back(shard, value.get());
      } else {
        touch(shard, it->second.get());
        value = it->second;  // lost the race: adopt the winner, ours closes
      }
      evict(shard);
    }
    co_return value;
  }

  void drop(const ObjId& oid) {
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    std::lock_guard lock(shard.mu);
    auto it = shard.entries.find(oid);
    if (it == shard.entries.end()) return;
    unlink(shard, it->second.get());
    shard.entries.erase(it);
  }

  size_t flush() {
    size_t dropped = 0;
    for (auto& shard : shards_) {
      std::lock_guard lock(shard.mu);
      for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (it->second.use_count() == 1) {
          unlink(shard, it->second.get());
          it = shard.entries.erase(it);
          ++dropped;
        } else {
          ++it;
        }
      }
    }
    return dropped;
  }

  void clear() {
    for (auto& shard : shards_) {
      std::lock_guard lock(shard.mu);
      shard.entries.clear();
      shard.lru_head = shard.lru_tail = nullptr;
    }
  }

  void fill(Stats& out) const {
    out.fd_hits = hits_.load(std::memory_order_relaxed);
    out.fd_misses = misses_.load(std::memory_order_relaxed);
    out.fd_upgrades = upgrades_.load(std::memory_order_relaxed);
    out.fd_evictions = evictions_.load(std::memory_order_relaxed);
    for (const auto& shard : shards_) {
      std::lock_guard lock(const_cast<std::mutex&>(shard.mu));
      out.fd_entries += shard.entries.size();
    }
  }

 private:
  static constexpr size_t kShards = 16;
  struct Shard {
    std::mutex mu;
    std::unordered_map<ObjId, Ref, ObjIdHash> entries;
    Entry* lru_head = nullptr;
    Entry* lru_tail = nullptr;
  };
  static void push_back(Shard& shard, Entry* e) {
    e->prev = shard.lru_tail;
    e->next = nullptr;
    (shard.lru_tail ? shard.lru_tail->next : shard.lru_head) = e;
    shard.lru_tail = e;
  }
  static void unlink(Shard& shard, Entry* e) {
    (e->prev ? e->prev->next : shard.lru_head) = e->next;
    (e->next ? e->next->prev : shard.lru_tail) = e->prev;
    e->prev = e->next = nullptr;
  }
  static void touch(Shard& shard, Entry* e) {
    if (shard.lru_tail == e) return;
    unlink(shard, e);
    push_back(shard, e);
  }
  void evict(Shard& shard) {
    size_t budget = shard.entries.size();
    while (shard.entries.size() > per_shard_capacity_ && budget-- > 0) {
      Entry* victim = shard.lru_head;
      auto it = shard.entries.find(victim->oid);
      if (it->second.use_count() == 1) {
        unlink(shard, victim);
        shard.entries.erase(it);
        evictions_.fetch_add(1, std::memory_order_relaxed);
      } else {
        touch(shard, victim);
      }
    }
  }

  CephBackend& backend_;
  size_t per_shard_capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0}, misses_{0}, upgrades_{0}, evictions_{0};
};

// Per-OPEN Fh (design 05 §5.5).  Only CephObject IO ever sees it (OpenCtx.open comes
// back from the same backend's open()), so the static_cast at the IO sites cannot
// cross backends.
class CephOpenState final : public OpenState {
 public:
  CephOpenState(const cephapi::Api* api, ceph_mount_info* mount, Fh* fh, bool writable)
      : api_(api), mount_(mount), fh_(fh), writable_(writable) {}
  ~CephOpenState() override {
    if (fh_) api_->ceph_ll_close(mount_, fh_);
  }
  Fh* fh() const { return fh_; }
  bool writable() const { return writable_; }

 private:
  const cephapi::Api* api_;
  ceph_mount_info* mount_;
  Fh* fh_;
  bool writable_;
};

namespace {
CephOpenState* open_state(const OpenCtx& ctx) {
  return static_cast<CephOpenState*>(ctx.open);
}
}  // namespace

// ---- backend -------------------------------------------------------------------

CephBackend::InodeRef::~InodeRef() {
  if (in) api->ceph_ll_put(mount, in);
}

CephBackend::CephBackend(Config cfg, std::shared_ptr<const cephapi::Api> api)
    : cfg_(std::move(cfg)), api_(std::move(api)) {
  caps_.set(Cap::kSymlink).set(Cap::kHardlink).set(Cap::kMknod);
  caps_.set(Cap::kStableHandles);  // inode numbers are never reused (06 §6.8)
  caps_.set(Cap::kNativeChange);   // stx_version: the MDS change attribute
  caps_.set(Cap::kSparseOps).set(Cap::kCopyRange);
  if (cfg_.jukebox) caps_.set(Cap::kJukebox);
  if (cfg_.native_locks) caps_.set(Cap::kByteLocks);
  fd_cache_ = std::make_unique<FdCache>(*this, cfg_.fd_cache);
  obj_cache_ = std::make_unique<ObjCache>(cfg_.fd_cache);
  if (cfg_.native_locks) locks_ = std::make_unique<CephLockMgr>(*this);
}

Result<std::unique_ptr<CephBackend>> CephBackend::create(
    Config cfg, std::shared_ptr<const cephapi::Api> api) {
  if (cfg.fsid == 0) return Err(errno_from(EINVAL));
  if (cfg.subdir.empty() || cfg.subdir.front() != '/') return Err(errno_from(EINVAL));
  for (const auto& [k, v] : cfg.options)
    if (k.empty() || k.find('=') != std::string::npos) return Err(errno_from(EINVAL));
  if (api && !cephapi::complete(*api)) return Err(errno_from(EINVAL));
  return std::unique_ptr<CephBackend>(new CephBackend(std::move(cfg), std::move(api)));
}

CephBackend::~CephBackend() {
  // stop() normally ran; this is the belt for a backend that never started or whose
  // stop() was skipped (tests).  Order: locks/fhs/inodes before unmount.
  if (locks_) locks_->close_all();
  fd_cache_.reset();
  obj_cache_.reset();
  root_.reset();
  if (root_perms_) api_->ceph_userperm_destroy(root_perms_);
  if (mount_) api_->ceph_shutdown(mount_);
}

std::optional<LockMgrRef> CephBackend::native_locks() {
  if (!locks_) return std::nullopt;
  return LockMgrRef(*locks_);
}

Errno CephBackend::map_rc(int64_t rc) const {
  int e = rc < 0 ? static_cast<int>(-rc) : 0;
  switch (e) {
    case ENOTCONN:
    case ETIMEDOUT:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EHOSTDOWN:
      // MDS failover / OSD reconnect / mon quorum loss: the cluster is expected back,
      // so ask the client to retry rather than fail its IO (kJukebox → v3 JUKEBOX /
      // v4 DELAY).  Off by config → EIO.
      if (cfg_.jukebox) {
        jukebox_.fetch_add(1, std::memory_order_relaxed);
        return Errno::kJukebox;
      }
      return errno_from(EIO);
    case kBlocklisted: {
      // The session was blocklisted (network partition, eviction): libcephfs will
      // never recover this mount on its own — a retry loop would spin forever, so
      // this is a hard EIO, counted for the operator (restart the gateway).
      uint64_t n = blocklisted_.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((n & (n - 1)) == 0)
        LNFS_ERROR("cephfs export {}: session blocklisted (EBLOCKLISTED, {} occurrences); "
                   "restart the gateway to reconnect", cfg_.subdir, n);
      return errno_from(EIO);
    }
    case 0: return errno_from(EIO);  // "failed" without a code
    default: return errno_from(e);
  }
}

Result<vinodeno_t> CephBackend::vino_from_oid(const ObjId& oid) {
  auto bytes = oid.view();
  if (bytes.size() != kObjIdLen || bytes[0] != kCephHandle) return Err(errno_from(ESTALE));
  vinodeno_t out{};
  std::memcpy(&out.ino, bytes.data() + 1, 8);
  std::memcpy(&out.snapid, bytes.data() + 9, 8);
  if (out.ino == 0) return Err(errno_from(ESTALE));  // no such inode number in Ceph
  return out;
}

ObjId CephBackend::oid_from_vino(const vinodeno_t& vino) {
  std::array<std::byte, kObjIdLen> encoded{};
  encoded[0] = kCephHandle;
  std::memcpy(encoded.data() + 1, &vino.ino, 8);
  std::memcpy(encoded.data() + 9, &vino.snapid, 8);
  return *ObjId::from(encoded);
}

ObjId CephBackend::oid_of(const struct ceph_statx& st) {
  // libcephfs reports the inode's snapid in stx_dev (Client::fill_statx); together
  // with stx_ino that is the vinodeno the MDS resolves.
  return oid_from_vino(vinodeno_t{st.stx_ino, static_cast<uint64_t>(st.stx_dev)});
}

Result<Attr> CephBackend::attr_from_statx(const struct ceph_statx& st) const {
  Attr a;
  a.type = mode_type(st.stx_mode);
  a.mode = st.stx_mode & 07777;
  a.nlink = st.stx_nlink;
  a.uid = st.stx_uid;
  a.gid = st.stx_gid;
  a.size = st.stx_size;
  a.used = st.stx_blocks * 512;
  a.rdev = DevT{static_cast<uint32_t>(major(st.stx_rdev)),
                static_cast<uint32_t>(minor(st.stx_rdev))};
  a.fileid = st.stx_ino;
  a.atime = convert_time(st.stx_atime);
  a.mtime = convert_time(st.stx_mtime);
  a.ctime = convert_time(st.stx_ctime);
  // The MDS change attribute (kNativeChange): bumped on every data or metadata
  // change, coherent across every client of the file — the "other half" of
  // multi-gateway consistency (09 册).  Missing from the mask (very old MDS) → the
  // 05 §5.6 ctime synthesis, so the counter never goes backwards on a mixed cluster.
  if (st.stx_mask & cephapi::kStatxVersion)
    a.change = st.stx_version;
  else
    a.change = static_cast<uint64_t>(std::max<int64_t>(a.ctime.sec, 0)) * 1000000000ull +
               a.ctime.nsec;
  return a;
}

Result<Attr> CephBackend::stat_sync(Inode* in, const UserPerm* perms) const {
  struct ceph_statx st {};
  int rc = api_->ceph_ll_getattr(mount_, in, &st, kWant, 0, perms ? perms : root_perms_);
  if (rc < 0) return Err(map_rc(rc));
  return attr_from_statx(st);
}

ObjPtr CephBackend::wrap(ObjRef ref, const ObjId& oid, FType type) {
  return std::static_pointer_cast<Object>(
      std::shared_ptr<CephObject>(new CephObject(*this, std::move(ref), oid, type)));
}

Result<ObjPtr> CephBackend::wrap_new(Inode* in, const struct ceph_statx& st) {
  auto ref = std::make_shared<InodeRef>(api_.get(), mount_, in);  // adopts the reference
  ObjId oid = oid_of(st);
  auto entry = obj_cache_->insert(
      oid, std::make_shared<ObjCache::Entry>(oid, ref, mode_type(st.stx_mode)));
  return wrap(entry->ref, oid, entry->type);
}

bool CephBackend::valid_name(std::string_view name, bool allow_dotdot) {
  if (name.empty() || name.find('/') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos)
    return false;
  if (!allow_dotdot && (name == "." || name == "..")) return false;
  return true;
}

rt::Task<Result<void>> CephBackend::start() {
  if (mount_) co_return Result<void>{};
  if (!api_) {
    std::string detail;
    auto loaded = cephapi::load_system_api(&detail);
    if (!loaded) {
      LNFS_ERROR("cephfs export {}: libcephfs unavailable: {}", cfg_.subdir, detail);
      co_return Err(loaded.error());
    }
    api_ = *loaded;
    fd_cache_ = std::make_unique<FdCache>(*this, cfg_.fd_cache);
  }
  // ceph_init/ceph_mount block on the monitors and the MDS: strictly offload material.
  auto cfg = cfg_;
  auto api = api_;
  struct Started {
    ceph_mount_info* mount;
    UserPerm* root_perms;
    Inode* root;
    struct ceph_statx st;
    std::string cluster_fsid;
    int64_t fscid;
    std::string version;
  };
  auto started = co_await rt::offload([cfg, api]() -> Result<Started> {
    ceph_mount_info* m = nullptr;
    int rc = api->ceph_create(&m, cfg.id.empty() ? nullptr : cfg.id.c_str());
    if (rc < 0 || !m) return Err(errno_from(rc < 0 ? -rc : ENOMEM));
    bool mounted = false;
    auto fail = [&](int e) {
      if (mounted) api->ceph_shutdown(m);
      else api->ceph_release(m);
      return Err(errno_from(e ? e : EIO));
    };
    rc = api->ceph_conf_read_file(m, cfg.conf.empty() ? nullptr : cfg.conf.c_str());
    // Without an explicit conf the library's default search may find nothing; the
    // mon_host/keyring keys (or $CEPH_ARGS) can still carry everything needed.
    if (rc < 0 && (!cfg.conf.empty() || rc != -ENOENT)) return fail(-rc);
    if (rc < 0) LNFS_WARN("cephfs export {}: no ceph.conf found, using defaults", cfg.subdir);
    auto set = [&](const char* key, const std::string& value) {
      if (value.empty()) return 0;
      return api->ceph_conf_set(m, key, value.c_str());
    };
    if ((rc = set("keyring", cfg.keyring)) < 0) return fail(-rc);
    if ((rc = set("mon_host", cfg.mon_host)) < 0) return fail(-rc);
    if ((rc = set("log_file", cfg.log_file)) < 0) return fail(-rc);
    for (const auto& [k, v] : cfg.options)
      if ((rc = api->ceph_conf_set(m, k.c_str(), v.c_str())) < 0) return fail(-rc);
    if ((rc = api->ceph_init(m)) < 0) return fail(-rc);
    if (!cfg.fs_name.empty() && (rc = api->ceph_select_filesystem(m, cfg.fs_name.c_str())) < 0)
      return fail(-rc);
    if ((rc = api->ceph_mount(m, cfg.subdir.c_str())) < 0) return fail(-rc);
    mounted = true;
    UserPerm* root_perms = api->ceph_userperm_new(0, 0, 0, nullptr);
    if (!root_perms) return fail(ENOMEM);
    Inode* root = nullptr;
    if ((rc = api->ceph_ll_lookup_root(m, &root)) < 0 || !root) {
      api->ceph_userperm_destroy(root_perms);
      return fail(rc < 0 ? -rc : ENOENT);
    }
    struct ceph_statx st {};
    if ((rc = api->ceph_ll_getattr(m, root, &st, kWant, 0, root_perms)) < 0) {
      api->ceph_ll_put(m, root);
      api->ceph_userperm_destroy(root_perms);
      return fail(-rc);
    }
    if (!S_ISDIR(st.stx_mode)) {
      api->ceph_ll_put(m, root);
      api->ceph_userperm_destroy(root_perms);
      return fail(ENOTDIR);
    }
    char buf[64] = {};
    std::string fsid;
    if (api->ceph_conf_get(m, "fsid", buf, sizeof buf) >= 0) fsid = buf;
    int major = 0, minor = 0, patch = 0;
    const char* ver = api->ceph_version(&major, &minor, &patch);
    return Started{m, root_perms, root, st, fsid, api->ceph_get_fs_cid(m),
                   ver ? ver : "?"};
  }, rt::OffloadClass::kHeavy);
  if (!started) {
    LNFS_ERROR("cephfs export {} (fs '{}'): mount failed: {}", cfg_.subdir, cfg_.fs_name,
               errno_name(started.error()));
    co_return Err(started.error());
  }
  mount_ = started->mount;
  root_perms_ = started->root_perms;
  root_ = std::make_shared<InodeRef>(api_.get(), mount_, started->root);
  root_oid_ = oid_of(started->st);
  cluster_fsid_ = started->cluster_fsid;
  fscid_ = started->fscid;
  obj_cache_->insert(root_oid_,
                     std::make_shared<ObjCache::Entry>(root_oid_, root_, FType::kDir));
  LNFS_INFO("cephfs export {} up: cluster {} fs '{}' (fscid {}) libcephfs {} "
            "native-locks={} jukebox={}",
            cfg_.subdir, cluster_fsid_.empty() ? "?" : cluster_fsid_,
            cfg_.fs_name.empty() ? "<default>" : cfg_.fs_name, fscid_, started->version,
            cfg_.native_locks, cfg_.jukebox);
  co_return Result<void>{};
}

rt::Task<Result<void>> CephBackend::stop() {
  if (!mount_) co_return Result<void>{};
  if (locks_) locks_->close_all();
  fd_cache_->clear();
  obj_cache_->clear();
  root_.reset();
  ceph_mount_info* m = mount_;
  mount_ = nullptr;
  UserPerm* perms = root_perms_;
  root_perms_ = nullptr;
  auto api = api_;
  co_await rt::offload([api, m, perms] {
    api->ceph_userperm_destroy(perms);
    api->ceph_unmount(m);
    api->ceph_release(m);
  }, rt::OffloadClass::kHeavy);
  co_return Result<void>{};
}

rt::Task<Result<ObjPtr>> CephBackend::root() {
  if (!mount_) co_return Err(errno_from(ENOTCONN));
  co_return wrap(root_, root_oid_, FType::kDir);
}

rt::Task<Result<ObjPtr>> CephBackend::resolve(const ObjId& oid) {
  if (!mount_) co_return Err(errno_from(ENOTCONN));
  auto vino = vino_from_oid(oid);
  if (!vino) co_return Err(vino.error());
  if (auto hit = obj_cache_->find(oid)) co_return wrap(hit->ref, oid, hit->type);
  auto got = co_await rt::offload([this, vino = *vino]() -> Result<std::pair<ObjRef, FType>> {
    Inode* in = nullptr;
    int rc = api_->ceph_ll_lookup_vino(mount_, vino, &in);
    if (rc < 0 || !in) {
      int e = rc < 0 ? -rc : ENOENT;
      if (e == ENOENT || e == ESTALE || e == EINVAL) return Err(errno_from(ESTALE));
      return Err(map_rc(rc));
    }
    auto ref = std::make_shared<InodeRef>(api_.get(), mount_, in);
    struct ceph_statx st {};
    rc = api_->ceph_ll_getattr(mount_, in, &st, kWant, 0, root_perms_);
    if (rc < 0) return Err(-rc == ENOENT ? errno_from(ESTALE) : map_rc(rc));
    return std::pair{ref, mode_type(st.stx_mode)};
  });
  if (!got) co_return Err(got.error());
  auto entry = obj_cache_->insert(
      oid, std::make_shared<ObjCache::Entry>(oid, got->first, got->second));
  co_return wrap(entry->ref, oid, entry->type);
}

rt::Task<Result<FsStats>> CephBackend::statfs() {
  if (!mount_) co_return Err(errno_from(ENOTCONN));
  auto root = root_;
  co_return co_await rt::offload([this, root]() -> Result<FsStats> {
    struct statvfs s {};
    int rc = api_->ceph_ll_statfs(mount_, root->in, &s);
    if (rc < 0) return Err(map_rc(rc));
    FsStats out;
    out.tbytes = static_cast<uint64_t>(s.f_blocks) * s.f_frsize;
    out.fbytes = static_cast<uint64_t>(s.f_bfree) * s.f_frsize;
    out.abytes = static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
    out.tfiles = s.f_files;
    out.ffiles = s.f_ffree;
    out.afiles = s.f_favail;
    return out;
  });
}

CephBackend::Stats CephBackend::stats() const {
  Stats out;
  if (fd_cache_) fd_cache_->fill(out);
  if (obj_cache_) {
    out.obj_hits = obj_cache_->hits();
    out.obj_misses = obj_cache_->misses();
    out.obj_entries = obj_cache_->entries();
  }
  out.jukebox = jukebox_.load(std::memory_order_relaxed);
  out.blocklisted = blocklisted_.load(std::memory_order_relaxed);
  if (locks_) out.lock_fds = locks_->fds();
  return out;
}

size_t CephBackend::flush_fd_cache() {
  return (fd_cache_ ? fd_cache_->flush() : 0) + (obj_cache_ ? obj_cache_->flush() : 0);
}

void CephBackend::poison(const ObjId& oid) {
  std::lock_guard lock(poison_mu_);
  poisoned_.insert(oid);
}

bool CephBackend::is_poisoned(const ObjId& oid) const {
  std::lock_guard lock(poison_mu_);
  return poisoned_.contains(oid);
}

size_t CephBackend::clear_poison() {
  std::lock_guard lock(poison_mu_);
  size_t n = poisoned_.size();
  poisoned_.clear();
  return n;
}

// ---- object: metadata -------------------------------------------------------

rt::Task<Result<Attr>> CephObject::getattr() {
  auto ref = ref_;
  co_return co_await rt::offload([this, ref] { return backend_.stat_sync(ref->in, nullptr); });
}

rt::Task<Result<void>> CephObject::require_dir(const Cred&) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  co_return Result<void>{};  // libcephfs enforces write permission on the directory
}

rt::Task<Result<ObjPtr>> CephObject::lookup(const Cred& cred, std::string_view name) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (!CephBackend::valid_name(name, true)) co_return Err(errno_from(EINVAL));
  if (name == "." || (name == ".." && id() == backend_.root_oid_))
    co_return backend_.wrap(ref_, id(), FType::kDir);
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, owned, oc]() -> Result<ObjPtr> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    struct ceph_statx st {};
    Inode* child = nullptr;
    int rc = api.ceph_ll_lookup(backend_.mount_, ref->in, owned.c_str(), &child, &st, kWant, 0,
                                perms.get());
    if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -ENOENT));
    return backend_.wrap_new(child, st);
  });
}

rt::Task<Result<Attr>> CephObject::setattr(const Cred& cred, const SetAttr& s) {
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, s]() -> Result<Attr> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    struct ceph_statx sb {};
    int mask = 0;
    if (s.size) {
      if (type() == FType::kDir) return Err(errno_from(EISDIR));
      if (type() != FType::kReg) return Err(errno_from(EINVAL));
      sb.stx_size = *s.size;
      mask |= cephapi::kSetSize;
    }
    if (s.mode) {
      sb.stx_mode = static_cast<uint16_t>(*s.mode & 07777);
      mask |= cephapi::kSetMode;
    }
    if (s.uid) {
      sb.stx_uid = *s.uid;
      mask |= cephapi::kSetUid;
    }
    if (s.gid) {
      sb.stx_gid = *s.gid;
      mask |= cephapi::kSetGid;
    }
    // "Server time" is the MDS clock (CEPH_SETATTR_*_NOW), not the gateway's.
    if (s.atime_how == SetAttr::TimeHow::kServer) mask |= cephapi::kSetAtimeNow;
    else if (s.atime_how == SetAttr::TimeHow::kClient) {
      sb.stx_atime = to_timespec(s.atime);
      mask |= cephapi::kSetAtime;
    }
    if (s.mtime_how == SetAttr::TimeHow::kServer) mask |= cephapi::kSetMtimeNow;
    else if (s.mtime_how == SetAttr::TimeHow::kClient) {
      sb.stx_mtime = to_timespec(s.mtime);
      mask |= cephapi::kSetMtime;
    }
    if (mask) {
      int rc = api.ceph_ll_setattr(backend_.mount_, ref->in, &sb, mask, perms.get());
      if (rc < 0 && !(type() == FType::kLnk && (mask & cephapi::kSetMode)))
        return Err(backend_.map_rc(rc));
    }
    return backend_.stat_sync(ref->in, perms.get());
  });
}

// ---- object: creation family ------------------------------------------------

Result<Created> CephObject::created_sync(Inode* child, struct ceph_statx st,
                                         const UserPerm* perms,
                                         std::optional<uint32_t> want_mode) {
  const auto& api = *backend_.api_;
  // A CREATE/MKDIR that named a mode must land exactly that mode: the MDS may apply
  // a umask callback / default ACL (the local backend does the same fchmod).
  if (want_mode && (st.stx_mode & 07777) != (*want_mode & 07777)) {
    struct ceph_statx sb {};
    sb.stx_mode = static_cast<uint16_t>(*want_mode & 07777);
    if (api.ceph_ll_setattr(backend_.mount_, child, &sb, cephapi::kSetMode, perms) == 0)
      st.stx_mode = static_cast<uint16_t>((st.stx_mode & ~07777u) | (*want_mode & 07777));
  }
  // The MDS reply to a create carries the attributes; a fresh getattr keeps the
  // change counter exact after the fix-ups above.
  struct ceph_statx cur {};
  if (api.ceph_ll_getattr(backend_.mount_, child, &cur, kWant, 0, perms) == 0) st = cur;
  auto attr = backend_.attr_from_statx(st);
  auto obj = backend_.wrap_new(child, st);  // adopts child
  if (!obj) return Err(obj.error());
  return Created{std::move(*obj), *attr};
}

rt::Task<Result<Created>> CephObject::create(const Cred& cred, std::string_view name,
                                             const SetAttr& attrs, ExclVerf* verf) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  ExclVerf verf_copy{};
  if (verf) verf_copy = *verf;
  bool exclusive = verf != nullptr;
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload(
      [this, ref, oc, owned, attrs, verf_copy, exclusive]() -> Result<Created> {
        const auto& api = *backend_.api_;
        Perms perms(api, oc);
        mode_t mode = exclusive ? 0 : (attrs.mode.value_or(0644) & 07777);
        struct ceph_statx st {};
        Inode* child = nullptr;
        Fh* fh = nullptr;
        int rc = api.ceph_ll_create(backend_.mount_, ref->in, owned.c_str(), mode,
                                    O_CREAT | O_EXCL | O_RDWR, &child, &fh, &st, kWant, 0,
                                    perms.get());
        if (rc == -EEXIST && exclusive) {
          // Retransmitted EXCLUSIVE create: the verifier lives in atime/mtime.
          rc = api.ceph_ll_lookup(backend_.mount_, ref->in, owned.c_str(), &child, &st, kWant,
                                  0, perms.get());
          if (rc == 0 && child && S_ISREG(st.stx_mode)) {
            int64_t lo = 0, hi = 0;
            verf_split(verf_copy, lo, hi);
            if (st.stx_atime.tv_sec == lo && st.stx_mtime.tv_sec == hi)
              return created_sync(child, st, perms.get(), std::nullopt);
          }
          if (rc == 0 && child) api.ceph_ll_put(backend_.mount_, child);
          return Err(errno_from(EEXIST));
        }
        if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
        if (fh) api.ceph_ll_close(backend_.mount_, fh);  // IO goes through the caches
        auto undo = [&](int64_t err) {
          api.ceph_ll_put(backend_.mount_, child);
          (void)api.ceph_ll_unlink(backend_.mount_, ref->in, owned.c_str(), perms.get());
          return Err(backend_.map_rc(err));
        };
        if (exclusive) {
          int64_t lo = 0, hi = 0;
          verf_split(verf_copy, lo, hi);
          struct ceph_statx sb {};
          sb.stx_atime = {static_cast<time_t>(lo), 0};
          sb.stx_mtime = {static_cast<time_t>(hi), 0};
          rc = api.ceph_ll_setattr(backend_.mount_, child, &sb,
                                   cephapi::kSetAtime | cephapi::kSetMtime, perms.get());
          if (rc < 0) return undo(rc);
          st.stx_atime = sb.stx_atime;
          st.stx_mtime = sb.stx_mtime;
          return created_sync(child, st, perms.get(), std::nullopt);
        }
        if (attrs.size) {
          struct ceph_statx sb {};
          sb.stx_size = *attrs.size;
          rc = api.ceph_ll_setattr(backend_.mount_, child, &sb, cephapi::kSetSize, perms.get());
          if (rc < 0) {
            int64_t e = rc;
            api.ceph_ll_put(backend_.mount_, child);
            return Err(backend_.map_rc(e));
          }
          st.stx_size = *attrs.size;
        }
        return created_sync(child, st, perms.get(), mode);
      });
}

rt::Task<Result<Created>> CephObject::mkdir(const Cred& cred, std::string_view name,
                                            const SetAttr& attrs) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  mode_t mode = attrs.mode.value_or(0755) & 07777;
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned, mode]() -> Result<Created> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    struct ceph_statx st {};
    Inode* child = nullptr;
    int rc = api.ceph_ll_mkdir(backend_.mount_, ref->in, owned.c_str(), mode, &child, &st, kWant,
                               0, perms.get());
    if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    return created_sync(child, st, perms.get(), mode);
  });
}

rt::Task<Result<Created>> CephObject::symlink(const Cred& cred, std::string_view name,
                                              std::string_view target, const SetAttr&) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  if (target.empty() || target.find('\0') != std::string_view::npos)
    co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name), owned_target(target);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned, owned_target]() -> Result<Created> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    struct ceph_statx st {};
    Inode* child = nullptr;
    int rc = api.ceph_ll_symlink(backend_.mount_, ref->in, owned.c_str(), owned_target.c_str(),
                                 &child, &st, kWant, 0, perms.get());
    if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    return created_sync(child, st, perms.get(), std::nullopt);
  });
}

rt::Task<Result<Created>> CephObject::mknod(const Cred& cred, std::string_view name,
                                            FType ftype, DevT dev, const SetAttr& attrs) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  mode_t type_bits = 0;
  switch (ftype) {
    case FType::kChr: type_bits = S_IFCHR; break;
    case FType::kBlk: type_bits = S_IFBLK; break;
    case FType::kSock: type_bits = S_IFSOCK; break;
    case FType::kFifo: type_bits = S_IFIFO; break;
    default: co_return Err(errno_from(EINVAL));
  }
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  mode_t mode = (attrs.mode.value_or(0644) & 07777) | type_bits;
  dev_t rdev = makedev(dev.major, dev.minor);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned, mode, rdev]() -> Result<Created> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    struct ceph_statx st {};
    Inode* child = nullptr;
    int rc = api.ceph_ll_mknod(backend_.mount_, ref->in, owned.c_str(), mode, rdev, &child, &st,
                               kWant, 0, perms.get());
    if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    return created_sync(child, st, perms.get(), mode & 07777);
  });
}

rt::Task<Result<void>> CephObject::unlink(const Cred& cred, std::string_view name) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    // ceph_ll_unlink on a directory answers EISDIR itself; the lookup is for the
    // Fh cache: no reason to keep a handle on a dead file.
    struct ceph_statx st {};
    Inode* child = nullptr;
    int rc = api.ceph_ll_lookup(backend_.mount_, ref->in, owned.c_str(), &child, &st, kWant, 0,
                                perms.get());
    if (rc < 0 || !child) return Err(backend_.map_rc(rc < 0 ? rc : -ENOENT));
    bool is_dir = S_ISDIR(st.stx_mode);
    ObjId oid = CephBackend::oid_of(st);
    api.ceph_ll_put(backend_.mount_, child);
    if (is_dir) return Err(errno_from(EISDIR));
    rc = api.ceph_ll_unlink(backend_.mount_, ref->in, owned.c_str(), perms.get());
    if (rc < 0) return Err(backend_.map_rc(rc));
    backend_.fd_cache_->drop(oid);
    return {};
  });
}

rt::Task<Result<void>> CephObject::rmdir(const Cred& cred, std::string_view name) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    int rc = api.ceph_ll_rmdir(backend_.mount_, ref->in, owned.c_str(), perms.get());
    if (rc < 0) return Err(backend_.map_rc(rc));  // ENOTDIR / ENOTEMPTY from the MDS
    return {};
  });
}

rt::Task<Result<void>> CephObject::rename(const Cred& cred, std::string_view from,
                                          Object& dst_dir, std::string_view to) {
  if (!CephBackend::valid_name(from) || !CephBackend::valid_name(to))
    co_return Err(errno_from(EINVAL));
  auto* dst = dynamic_cast<CephObject*>(&dst_dir);
  if (!dst || &dst->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto src_ok = co_await require_dir(cred);
  if (!src_ok) co_return Err(src_ok.error());
  auto dst_ok = co_await dst->require_dir(cred);
  if (!dst_ok) co_return Err(dst_ok.error());
  std::string owned_from(from), owned_to(to);
  OwnedCred oc(cred);
  auto sref = ref_, dref = dst->ref_;
  co_return co_await rt::offload([this, sref, dref, oc, owned_from, owned_to]() -> Result<void> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    int rc = api.ceph_ll_rename(backend_.mount_, sref->in, owned_from.c_str(), dref->in,
                                owned_to.c_str(), perms.get());
    if (rc < 0) return Err(backend_.map_rc(rc));
    return {};
  });
}

rt::Task<Result<void>> CephObject::link(const Cred& cred, Object& file,
                                        std::string_view name) {
  if (!CephBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto* target = dynamic_cast<CephObject*>(&file);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (target->type() == FType::kDir) co_return Err(errno_from(EPERM));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto dref = ref_, tref = target->ref_;
  co_return co_await rt::offload([this, dref, tref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    int rc = api.ceph_ll_link(backend_.mount_, tref->in, dref->in, owned.c_str(), perms.get());
    if (rc < 0) return Err(backend_.map_rc(rc));
    return {};
  });
}

rt::Task<Result<DirPage>> CephObject::readdir(const Cred& cred, uint64_t cookie,
                                              uint32_t max_entries) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (max_entries == 0) co_return DirPage{};
  OwnedCred oc(cred);
  auto ref = ref_;
  bool enrich = backend_.cfg_.enrich_readdir;
  co_return co_await rt::offload([this, ref, oc, cookie, max_entries, enrich]() -> Result<DirPage> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    // One directory handle per page: libcephfs keeps the position in the
    // ceph_dir_result and every page is an independent request; a shared handle
    // would need the serialization the local backend's dents mutex provides — at MDS
    // latency the opendir/releasedir pair is the cheaper price.
    ceph_dir_result* dir = nullptr;
    int rc = api.ceph_ll_opendir(backend_.mount_, ref->in, &dir, perms.get());
    if (rc < 0 || !dir) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    struct Closer {
      const cephapi::Api& api;
      ceph_mount_info* mount;
      ceph_dir_result* dir;
      ~Closer() { api.ceph_ll_releasedir(mount, dir); }
    } closer{api, backend_.mount_, dir};
    if (cookie != 0) api.ceph_seekdir(backend_.mount_, dir, static_cast<int64_t>(cookie));
    DirPage page;
    while (page.ents.size() < max_entries) {
      struct dirent de {};
      struct ceph_statx st {};
      // The readdir reply already carries every entry's attributes (readdirplus is
      // one MDS round trip per batch), so DONT_SYNC costs nothing and skips a second
      // trip; no Inode reference is taken (out = nullptr) — the ObjId comes from the
      // statx alone.
      rc = api.ceph_readdirplus_r(backend_.mount_, dir, &de, &st, kWant,
                                  cephapi::kStatxDontSync, nullptr);
      if (rc < 0) return Err(backend_.map_rc(rc));
      if (rc == 0) {
        page.eof = true;
        break;
      }
      std::string_view name(de.d_name);
      if (name == "." || name == "..") continue;
      DirPage::Ent out{.name = std::string(name),
                       .cookie = static_cast<uint64_t>(de.d_off),
                       .fileid = st.stx_ino ? st.stx_ino : de.d_ino,
                       .attr = std::nullopt,
                       .oid = std::nullopt};
      if (enrich && (st.stx_mask & cephapi::kStatxIno)) {
        auto attr = backend_.attr_from_statx(st);
        if (attr) out.attr = *attr;
        out.oid = CephBackend::oid_of(st);
      }
      page.ents.push_back(std::move(out));
    }
    return page;
  });
}

rt::Task<Result<std::string>> CephObject::readlink() {
  if (type() != FType::kLnk) co_return Err(errno_from(EINVAL));
  auto ref = ref_;
  co_return co_await rt::offload([this, ref]() -> Result<std::string> {
    std::vector<char> buf(4096);
    int n = backend_.api_->ceph_ll_readlink(backend_.mount_, ref->in, buf.data(), buf.size(),
                                            backend_.root_perms_);
    if (n < 0) return Err(backend_.map_rc(n));
    if (static_cast<size_t>(n) >= buf.size()) return Err(errno_from(ENAMETOOLONG));
    return std::string(buf.data(), static_cast<size_t>(n));
  });
}

// ---- object: IO ---------------------------------------------------------------

rt::Task<Result<void>> CephObject::io_gate(const Cred& cred, bool write) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto allowed = co_await access(cred, write ? Access::kModify : Access::kRead);
  if (!allowed) co_return Err(allowed.error());
  if (!allowed->has(write ? Access::kModify : Access::kRead)) {
    // v3 open-less owner relaxation (nfsv3/04 §6): the owner may read/write a file
    // whose mode bits deny it, as a local open(2) by the owner would after chmod.
    auto attr = co_await getattr();
    if (!attr) co_return Err(attr.error());
    if (cred.uid != attr->uid) co_return Err(errno_from(EACCES));
  }
  co_return Result<void>{};
}

rt::Task<Result<OpenPtr>> CephObject::open(const Cred& cred, OpenFlags flags) {
  // libcephfs authorizes this open under the caller's identity (client_permissions):
  // an EACCES here is the OPEN's answer, no EOPNOTSUPP degrade.
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  bool writable = flags.has(OpenFlag::kWrite);
  int oflags = writable ? O_RDWR : O_RDONLY;
  if (flags.has(OpenFlag::kTruncate) && writable) oflags |= O_TRUNC;
  OwnedCred oc(cred);
  auto ref = ref_;
  auto opened = co_await rt::offload([this, ref, oc, oflags]() -> Result<Fh*> {
    const auto& api = *backend_.api_;
    Perms perms(api, oc);
    Fh* fh = nullptr;
    int rc = api.ceph_ll_open(backend_.mount_, ref->in, oflags, &fh, perms.get());
    if (rc < 0 || !fh) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    return fh;
  });
  if (!opened) co_return Err(opened.error());
  co_return OpenPtr(
      std::make_shared<CephOpenState>(backend_.api_.get(), backend_.mount_, *opened, writable));
}

namespace {
rt::Task<void> maybe_slow_io() {
  if (fault::take(fault::Kind::kSlowIo))
    co_await rt::sleep_for(std::chrono::milliseconds(fault::slow_ms()));
}
}  // namespace

rt::Task<Result<uint32_t>> CephObject::read(OpenCtx ctx, uint64_t off,
                                            std::span<std::byte> out, bool& eof) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  Fh* fh = nullptr;
  CephBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx)) {
    fh = os->fh();
  } else {
    auto gate = co_await io_gate(ctx.cred, false);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fh = ref->fh;
  }
  co_await maybe_slow_io();
  if (fault::take(fault::Kind::kReadEio)) co_return Err(errno_from(EIO));
  if (fault::take(fault::Kind::kJukebox)) co_return Err(Errno::kJukebox);
  auto n = co_await rt::offload([this, fh, off, out]() -> Result<uint32_t> {
    if (out.empty()) return 0u;
    int r = backend_.api_->ceph_ll_read(backend_.mount_, fh, static_cast<int64_t>(off),
                                        out.size(), reinterpret_cast<char*>(out.data()));
    if (r < 0) return Err(backend_.map_rc(r));
    return static_cast<uint32_t>(r);
  });
  if (!n) co_return Err(n.error());
  if (out.empty()) {
    auto attr = co_await getattr();
    eof = attr && off >= attr->size;
  } else {
    eof = *n < out.size();
  }
  co_return *n;
}

rt::Task<Result<uint32_t>> CephObject::write(OpenCtx ctx, uint64_t off,
                                             std::span<const std::byte> in,
                                             Stability stability) {
  iovec v{const_cast<std::byte*>(in.data()), in.size()};
  co_return co_await write(ctx, off, std::span<const iovec>(&v, 1), stability);
}

rt::Task<Result<uint32_t>> CephObject::write(OpenCtx ctx, uint64_t off,
                                             std::span<const iovec> iov,
                                             Stability stability) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  Fh* fh = nullptr;
  CephBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx); os && os->writable()) {
    fh = os->fh();
  } else {
    auto gate = co_await io_gate(ctx.cred, true);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, true);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fh = ref->fh;
  }
  SmallVec<iovec, 16> vec;
  size_t total = 0;
  for (const auto& v : iov) {
    if (v.iov_len == 0) continue;
    vec.push_back(v);
    total += v.iov_len;
  }
  co_await maybe_slow_io();
  if (fault::take(fault::Kind::kWriteEnospc)) co_return Err(errno_from(ENOSPC));
  if (fault::take(fault::Kind::kWriteEdquot)) co_return Err(errno_from(EDQUOT));
  if (fault::take(fault::Kind::kJukebox)) co_return Err(Errno::kJukebox);
  bool short_write = fault::take(fault::Kind::kShortWrite);
  bool sync_fault = stability != Stability::kUnstable && fault::take(fault::Kind::kFsyncEio);
  auto done = co_await rt::offload([this, fh, off, vec, total, stability, short_write,
                                    sync_fault]() mutable -> Result<uint32_t> {
    const auto& api = *backend_.api_;
    size_t done = 0, idx = 0;
    while (done < total) {
      int64_t n;
      if (short_write) {  // fault: 1 byte, exercising the iovec advance below
        short_write = false;
        n = api.ceph_ll_write(backend_.mount_, fh, static_cast<int64_t>(off + done), 1,
                              static_cast<const char*>(vec[idx].iov_base));
      } else {
        n = api.ceph_ll_writev(backend_.mount_, fh, vec.data() + idx,
                               static_cast<int>(vec.size() - idx),
                               static_cast<int64_t>(off + done));
      }
      if (n < 0) return Err(backend_.map_rc(n));
      if (n == 0) return Err(errno_from(EIO));
      done += static_cast<size_t>(n);
      size_t adv = static_cast<size_t>(n);
      while (idx < vec.size() && adv >= vec[idx].iov_len) {
        adv -= vec[idx].iov_len;
        ++idx;
      }
      if (idx < vec.size() && adv > 0) {
        vec[idx].iov_base = static_cast<char*>(vec[idx].iov_base) + adv;
        vec[idx].iov_len -= adv;
      }
    }
    if (stability != Stability::kUnstable) {
      int rc = sync_fault ? -EIO
                          : api.ceph_ll_fsync(backend_.mount_, fh,
                                              stability == Stability::kDataSync ? 1 : 0);
      if (rc < 0) {
        backend_.poison(id());
        return Err(sync_fault ? errno_from(EIO) : backend_.map_rc(rc));
      }
    }
    return static_cast<uint32_t>(done);
  }, stability == Stability::kUnstable ? rt::OffloadClass::kLight : rt::OffloadClass::kHeavy);
  co_return done;
}

rt::Task<Result<void>> CephObject::commit(OpenCtx ctx, uint64_t, uint64_t) {
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  if (backend_.is_poisoned(id())) co_return Err(errno_from(EIO));  // sticky (06 §6.2)
  Fh* fh = nullptr;
  CephBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx); os && os->writable()) {
    fh = os->fh();
  } else {
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fh = ref->fh;
  }
  bool sync_fault = fault::take(fault::Kind::kFsyncEio);
  co_return co_await rt::offload([this, fh, sync_fault]() -> Result<void> {
    int rc = sync_fault ? -EIO : backend_.api_->ceph_ll_fsync(backend_.mount_, fh, 1);
    if (rc < 0) {
      backend_.poison(id());
      return Err(sync_fault ? errno_from(EIO) : backend_.map_rc(rc));
    }
    return {};
  }, rt::OffloadClass::kHeavy);
}

// ---- object: v4.2 -------------------------------------------------------------

rt::Task<Result<uint64_t>> CephObject::seek(OpenCtx ctx, uint64_t off, SeekWhat what) {
  Fh* fh = nullptr;
  CephBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx)) {
    fh = os->fh();
  } else {
    auto gate = co_await io_gate(ctx.cred, false);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fh = ref->fh;
  }
  co_return co_await rt::offload([this, fh, off, what]() -> Result<uint64_t> {
    // Ceph has no extent map for SEEK_DATA/HOLE: data = the offset itself while
    // inside the file, hole = EOF; past EOF both answer ENXIO — exactly the RFC 7862
    // §15.11 minimum ("the whole file is data").
    off_t r = backend_.api_->ceph_ll_lseek(backend_.mount_, fh, static_cast<off_t>(off),
                                           what == SeekWhat::kData ? SEEK_DATA : SEEK_HOLE);
    if (r < 0) return Err(backend_.map_rc(r));
    return static_cast<uint64_t>(r);
  });
}

rt::Task<Result<void>> CephObject::allocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(*this, true);
  if (!ref) co_return Err(ref.error());
  Fh* fh = (*ref)->fh;
  co_return co_await rt::offload([this, fh, off, len]() -> Result<void> {
    int rc = backend_.api_->ceph_ll_fallocate(backend_.mount_, fh, 0, static_cast<int64_t>(off),
                                              static_cast<int64_t>(len));
    if (rc < 0) return Err(backend_.map_rc(rc));
    return {};
  }, rt::OffloadClass::kHeavy);
}

rt::Task<Result<void>> CephObject::deallocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(*this, true);
  if (!ref) co_return Err(ref.error());
  Fh* fh = (*ref)->fh;
  co_return co_await rt::offload([this, fh, off, len]() -> Result<void> {
    int rc = backend_.api_->ceph_ll_fallocate(backend_.mount_, fh,
                                              cephapi::kFallocPunchHole | cephapi::kFallocKeepSize,
                                              static_cast<int64_t>(off),
                                              static_cast<int64_t>(len));
    if (rc < 0) return Err(backend_.map_rc(rc));
    return {};
  }, rt::OffloadClass::kHeavy);
}

rt::Task<Result<uint64_t>> CephObject::copy_range(OpenCtx sctx, Object& dst, OpenCtx dctx,
                                                  uint64_t src_off, uint64_t dst_off,
                                                  uint64_t len) {
  auto* target = dynamic_cast<CephObject*>(&dst);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto sgate = co_await io_gate(sctx.cred, false);
  if (!sgate) co_return Err(sgate.error());
  auto dgate = co_await target->io_gate(dctx.cred, true);
  if (!dgate) co_return Err(dgate.error());
  auto sref = co_await backend_.fd_cache_->acquire(*this, false);
  if (!sref) co_return Err(sref.error());
  auto dref = co_await backend_.fd_cache_->acquire(*target, true);
  if (!dref) co_return Err(dref.error());
  Fh* sfh = (*sref)->fh;
  Fh* dfh = (*dref)->fh;
  auto src_in = ref_;
  co_return co_await rt::offload([this, src_in, sfh, dfh, src_off, dst_off, len]() -> Result<uint64_t> {
    // libcephfs has no copy_file_range: the copy is a read/write loop on the gateway
    // (still server-side from the client's point of view — no NFS traffic).
    const auto& api = *backend_.api_;
    uint64_t want = len;
    if (want == 0) {  // to EOF
      auto attr = backend_.stat_sync(src_in->in, nullptr);
      if (!attr) return Err(attr.error());
      if (attr->size <= src_off) return 0;
      want = attr->size - src_off;
    }
    std::vector<std::byte> buf(static_cast<size_t>(std::min<uint64_t>(want, 1ull << 20)));
    uint64_t done = 0;
    while (done < want) {
      size_t chunk = static_cast<size_t>(std::min<uint64_t>(want - done, buf.size()));
      int r = api.ceph_ll_read(backend_.mount_, sfh, static_cast<int64_t>(src_off + done), chunk,
                               reinterpret_cast<char*>(buf.data()));
      if (r < 0) return Err(backend_.map_rc(r));
      if (r == 0) break;
      size_t w = 0;
      while (w < static_cast<size_t>(r)) {
        int k = api.ceph_ll_write(backend_.mount_, dfh, static_cast<int64_t>(dst_off + done + w),
                                  static_cast<size_t>(r) - w,
                                  reinterpret_cast<const char*>(buf.data()) + w);
        if (k < 0) return Err(backend_.map_rc(k));
        if (k == 0) return Err(errno_from(EIO));
        w += static_cast<size_t>(k);
      }
      done += static_cast<uint64_t>(r);
    }
    return done;
  }, rt::OffloadClass::kHeavy);
}

// ---- native byte-range locks ---------------------------------------------------

size_t CephLockMgr::KeyHash::operator()(const Key& k) const noexcept {
  return ObjIdHash{}(k.oid) ^ (std::hash<std::string>{}(k.owner) << 1);
}

uint64_t CephLockMgr::owner_key(const LockOwnerId& owner) {
  // FNV-1a over the owner bytes: the state layer's lock-owner ids are short opaque
  // strings (clientid + owner name digest), a 64-bit hash keeps them distinct with
  // overwhelming probability and never collides with the probe owner (0).
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t i = 0; i < owner.len; ++i) {
    h ^= static_cast<uint8_t>(owner.bytes[i]);
    h *= 0x100000001b3ull;
  }
  return h ? h : 1;
}

CephLockMgr::~CephLockMgr() { close_all(); }

void CephLockMgr::close_all() {
  std::lock_guard lock(mu_);
  for (auto& [key, fh] : fhs_) backend_.api_->ceph_ll_close(backend_.mount_, fh);
  fhs_.clear();
}

size_t CephLockMgr::fds() const {
  std::lock_guard lock(const_cast<std::mutex&>(mu_));
  return fhs_.size();
}

struct flock CephLockMgr::make_flock(LockRange range, short type) {
  struct flock fl {};
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(range.offset);
  // Wire "to EOF" (length ~0 / UINT64_MAX) is l_len 0; anything overflowing off_t
  // is clamped to EOF as well (the range cannot be expressed more precisely).
  uint64_t end = range.length == UINT64_MAX ? UINT64_MAX : range.offset + range.length;
  if (range.length == 0 || range.length == UINT64_MAX ||
      end < range.offset || end > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    fl.l_len = 0;
  else
    fl.l_len = static_cast<off_t>(range.length);
  return fl;
}

// Runs on an offload worker.  Lock handles are opened as the gateway (uid 0): the
// state layer already checked the OPEN's access mode covers the lock type (RFC 8881
// §18.10.3), and the MDS does not re-check permissions per fcntl.
Result<Fh*> CephLockMgr::fh_for(CephObject& obj, const LockOwnerId& owner, bool create) {
  Key key{obj.id(), std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
  {
    std::lock_guard lock(mu_);
    auto it = fhs_.find(key);
    if (it != fhs_.end()) return it->second;
  }
  if (!create) return Err(errno_from(ENOENT));
  const auto& api = *backend_.api_;
  Fh* fh = nullptr;
  int rc = api.ceph_ll_open(backend_.mount_, obj.handle(), O_RDWR, &fh, backend_.root_perms_);
  if (rc < 0 && (rc == -EACCES || rc == -EROFS || rc == -EPERM))
    rc = api.ceph_ll_open(backend_.mount_, obj.handle(), O_RDONLY, &fh, backend_.root_perms_);
  if (rc < 0 || !fh) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
  std::lock_guard lock(mu_);
  auto [it, inserted] = fhs_.emplace(key, fh);
  if (!inserted) {  // lost a race: keep the winner
    api.ceph_ll_close(backend_.mount_, fh);
    return it->second;
  }
  return fh;
}

rt::Task<Result<void>> CephLockMgr::lock(Object& object, const LockOwnerId& owner,
                                         LockRange range, bool exclusive, bool wait) {
  auto* obj = dynamic_cast<CephObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner, range, exclusive, wait]() -> Result<void> {
    auto fh = fh_for(*obj, owner, true);
    if (!fh) return Err(fh.error());
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    // The state layer never blocks a request on a lock (RFC 8881 §18.10: clients
    // poll / get CB_NOTIFY_LOCK), so `wait` is accepted but not honoured (sleep = 0).
    (void)wait;
    int rc = backend_.api_->ceph_ll_setlk(backend_.mount_, *fh, &fl, owner_key(owner), 0);
    if (rc < 0) {
      if (rc == -EAGAIN || rc == -EWOULDBLOCK || rc == -EACCES) return Err(errno_from(EAGAIN));
      return Err(backend_.map_rc(rc));
    }
    return {};
  });
}

rt::Task<Result<void>> CephLockMgr::unlock(Object& object, const LockOwnerId& owner,
                                           LockRange range) {
  auto* obj = dynamic_cast<CephObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner, range]() -> Result<void> {
    auto fh = fh_for(*obj, owner, false);
    if (!fh) return {};  // nothing held by this owner on this file: unlocking is idempotent
    struct flock fl = make_flock(range, F_UNLCK);
    int rc = backend_.api_->ceph_ll_setlk(backend_.mount_, *fh, &fl, owner_key(owner), 0);
    if (rc < 0) return Err(backend_.map_rc(rc));
    return {};
  });
}

rt::Task<Result<std::optional<LockConflict>>> CephLockMgr::test(Object& object,
                                                                LockRange range,
                                                                bool exclusive) {
  auto* obj = dynamic_cast<CephObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, range, exclusive]() -> Result<std::optional<LockConflict>> {
    // A probe handle under an owner no client can have (0: owner_key never yields
    // it): GETLK reports any conflicting holder, local or on another gateway.
    const auto& api = *backend_.api_;
    Fh* fh = nullptr;
    int rc = api.ceph_ll_open(backend_.mount_, obj->handle(), O_RDONLY, &fh, backend_.root_perms_);
    if (rc < 0 || !fh) return Err(backend_.map_rc(rc < 0 ? rc : -EIO));
    struct Closer {
      const cephapi::Api& api;
      ceph_mount_info* mount;
      Fh* fh;
      ~Closer() { api.ceph_ll_close(mount, fh); }
    } closer{api, backend_.mount_, fh};
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    rc = api.ceph_ll_getlk(backend_.mount_, fh, &fl, 0);
    if (rc < 0) return Err(backend_.map_rc(rc));
    if (fl.l_type == F_UNLCK) return std::optional<LockConflict>{};
    LockConflict c;
    c.exclusive = fl.l_type == F_WRLCK;
    c.range.offset = static_cast<uint64_t>(fl.l_start);
    c.range.length = fl.l_len == 0 ? UINT64_MAX : static_cast<uint64_t>(fl.l_len);
    // The holder's owner is not reported by GETLK (only its pid); an empty owner
    // tells the state layer "someone else, possibly on another gateway".
    return std::optional<LockConflict>(c);
  });
}

rt::Task<Result<void>> CephLockMgr::release(Object& object, const LockOwnerId& owner) {
  auto* obj = dynamic_cast<CephObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner]() -> Result<void> {
    Key key{obj->id(),
            std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
    Fh* fh = nullptr;
    {
      std::lock_guard lock(mu_);
      auto it = fhs_.find(key);
      if (it == fhs_.end()) return {};
      fh = it->second;
      fhs_.erase(it);
    }
    // Closing the Fh releases every lock it holds (Client::_release_filelocks); the
    // explicit full-range unlock first keeps the MDS view exact even when the close
    // is deferred behind a pending request.
    struct flock fl = make_flock({0, UINT64_MAX}, F_UNLCK);
    (void)backend_.api_->ceph_ll_setlk(backend_.mount_, fh, &fl, owner_key(owner), 0);
    backend_.api_->ceph_ll_close(backend_.mount_, fh);
    return {};
  });
}

// ---- factory ---------------------------------------------------------------------

namespace {

bool parse_bool(const std::string& value, bool& out) {
  if (value != "true" && value != "false") return false;
  out = value == "true";
  return true;
}

template <class T>
bool parse_uint(const std::string& value, T& out) {
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
  return ec == std::errc{} && ptr == value.data() + value.size();
}

// "client_mount_timeout=30,client_cache_size=32768" → options
bool parse_options(const std::string& value,
                   std::vector<std::pair<std::string, std::string>>& out) {
  size_t pos = 0;
  while (pos <= value.size()) {
    size_t comma = value.find(',', pos);
    std::string item = value.substr(pos, comma == std::string::npos ? std::string::npos
                                                                     : comma - pos);
    pos = comma == std::string::npos ? value.size() + 1 : comma + 1;
    while (!item.empty() && item.back() == ' ') item.pop_back();
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    if (item.empty()) continue;
    size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
  }
  return true;
}

// A mistyped key or value fails startup (same policy as the local backend).
std::unique_ptr<Backend> make_cephfs(const BackendConfig& cfg) {
  CephBackend::Config c;
  c.fsid = cfg.fsid;
  auto bad = [&](const char* key, const std::string& value) {
    LNFS_ERROR("export {}: bad cephfs backend {} value '{}'", cfg.path, key, value);
    return std::unique_ptr<Backend>{};
  };
  for (const auto& [key, value] : cfg.values) {
    if (key == "conf") c.conf = value;
    else if (key == "id" || key == "user" || key == "name") c.id = value;
    else if (key == "keyring") c.keyring = value;
    else if (key == "mon_host" || key == "monitors") c.mon_host = value;
    else if (key == "fs_name" || key == "filesystem") c.fs_name = value;
    else if (key == "subdir") c.subdir = value;
    else if (key == "log_file") c.log_file = value;
    else if (key == "options") {
      if (!parse_options(value, c.options)) return bad("options", value);
    } else if (key == "fd_cache") {
      if (!parse_uint(value, c.fd_cache) || c.fd_cache == 0) return bad("fd_cache", value);
    } else if (key == "readdir_enrich") {
      if (!parse_bool(value, c.enrich_readdir)) return bad("readdir_enrich", value);
    } else if (key == "jukebox") {
      if (!parse_bool(value, c.jukebox)) return bad("jukebox", value);
    } else if (key == "native_locks") {
      if (!parse_bool(value, c.native_locks)) return bad("native_locks", value);
    } else {
      LNFS_ERROR("export {}: unknown cephfs backend key '{}'", cfg.path, key);
      return nullptr;
    }
  }
  auto made = CephBackend::create(std::move(c));
  if (!made) {
    LNFS_ERROR("export {}: cephfs backend config rejected: {}", cfg.path,
               errno_name(made.error()));
    return nullptr;
  }
  return std::move(*made);
}

}  // namespace

void register_cephfs_backend() {
  register_backend({"cephfs", kBackendApiVersion, make_cephfs, /*virtual_path=*/true});
}

}  // namespace lnfs::backend
