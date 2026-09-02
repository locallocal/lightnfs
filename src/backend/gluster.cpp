#include "backend/gluster.hpp"

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

constexpr std::byte kGfidHandle{3};  // ObjId tag (local uses 1 = kernel, 2 = fallback)
constexpr size_t kObjIdLen = 1 + gfapi::kHandleLength;

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

// Caller identity for the duration of one libgfapi call (design 06 §6.6 "透传给存储侧
// 鉴权"): glfs_setfs* are thread-local in libgfapi, so this is set on the offload
// worker right before the call and reset after it.  Supplementary groups are copied
// out of the request because the Cred's span points into the caller's frame.
class ScopedIds {
 public:
  ScopedIds(const gfapi::Api& api, uint32_t uid, uint32_t gid,
            std::span<const uint32_t> gids)
      : api_(api) {
    api_.glfs_setfsuid(uid);
    api_.glfs_setfsgid(gid);
    SmallVec<gid_t, 32> groups;
    for (uint32_t g : gids) groups.push_back(g);
    api_.glfs_setfsgroups(groups.size(), groups.size() ? groups.data() : nullptr);
  }
  ~ScopedIds() {
    api_.glfs_setfsuid(0);
    api_.glfs_setfsgid(0);
    api_.glfs_setfsgroups(0, nullptr);
  }
  ScopedIds(const ScopedIds&) = delete;

 private:
  const gfapi::Api& api_;
};

// A Cred whose group list survives a hop to the offload pool.
struct OwnedCred {
  uint32_t uid, gid;
  SmallVec<uint32_t, 32> gids;
  explicit OwnedCred(const Cred& c) : uid(c.uid), gid(c.gid) {
    for (uint32_t g : c.gids) gids.push_back(g);
  }
  std::span<const uint32_t> groups() const { return {gids.data(), gids.size()}; }
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

// ---- object-handle cache -----------------------------------------------------

// ObjId → live glfs_object (plan doc 10 §2.1 analogue of the O_PATH resolve cache):
// resolve() is hit at the top of every request; a hit costs a shard mutex instead of a
// glfs_h_create_from_handle round trip to the bricks.  Entries pin the inode in
// libgfapi's inode table until eviction — the same bounded-staleness trade as local.
class GlusterBackend::ObjCache {
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

  void erase(const ObjId& oid) {
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

// ---- anonymous-IO glfd cache ----------------------------------------------------

// Same shape as the local backend's FdCache (06 §6.3): one glfd per object, read
// acquires reuse anything, write acquires need O_RDWR and upgrade in place.  The
// descriptor is opened as the gateway itself (fsuid 0), not as the requester: the
// per-IO gate (native access() + the v3 owner relaxation) has already decided, and a
// shared descriptor must not carry the first requester's identity to later ones.
// The v4 OPEN path (GlusterObject::open) is the one that opens under the caller.
class GlusterBackend::FdCache {
 public:
  struct Entry {
    Entry(const gfapi::Api* a, const ObjId& id, glfs_fd* value, int mode)
        : api(a), oid(id), fd(value), accmode(mode) {}
    ~Entry() {
      if (fd) api->glfs_close(fd);
    }
    const gfapi::Api* api;
    ObjId oid;
    glfs_fd* fd;
    int accmode;
    Entry* prev = nullptr;
    Entry* next = nullptr;
  };
  using Ref = std::shared_ptr<Entry>;

  FdCache(GlusterBackend& backend, size_t capacity)
      : backend_(backend),
        per_shard_capacity_(std::max<size_t>((capacity + kShards - 1) / kShards, 1)) {}

  rt::Task<Result<Ref>> acquire(GlusterObject& obj, bool write) {
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
    auto ref = obj.ref_;  // keeps the glfs_object alive across the hop
    auto opened = co_await rt::offload([this, ref, flags]() -> Result<glfs_fd*> {
      const auto& api = *backend_.api_;
      ScopedIds ids(api, 0, 0, {});
      glfs_fd* fd = api.glfs_h_open(backend_.fs_, ref->obj, flags);
      if (!fd) return Err(backend_.map_errno(errno));
      return fd;
    });
    if (!opened) co_return Err(opened.error());
    auto value = std::make_shared<Entry>(backend_.api_.get(), oid, *opened, flags);
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

  GlusterBackend& backend_;
  size_t per_shard_capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0}, misses_{0}, upgrades_{0}, evictions_{0};
};

// Per-OPEN glfd (design 05 §5.5): the second real producer of OpenState after the
// local backend.  Only GlusterObject IO ever sees it (OpenCtx.open comes back from the
// same backend's open()), so the static_cast at the IO sites cannot cross backends.
class GlusterOpenState final : public OpenState {
 public:
  GlusterOpenState(const gfapi::Api* api, glfs_fd* fd, bool writable)
      : api_(api), fd_(fd), writable_(writable) {}
  ~GlusterOpenState() override {
    if (fd_) api_->glfs_close(fd_);
  }
  glfs_fd* fd() const { return fd_; }
  bool writable() const { return writable_; }

 private:
  const gfapi::Api* api_;
  glfs_fd* fd_;
  bool writable_;
};

namespace {
GlusterOpenState* open_state(const OpenCtx& ctx) {
  return static_cast<GlusterOpenState*>(ctx.open);
}
}  // namespace

// ---- backend -------------------------------------------------------------------

GlusterBackend::ObjHandle::~ObjHandle() {
  if (obj) api->glfs_h_close(obj);
}

GlusterBackend::GlusterBackend(Config cfg, std::shared_ptr<const gfapi::Api> api)
    : cfg_(std::move(cfg)), api_(std::move(api)) {
  caps_.set(Cap::kSymlink).set(Cap::kHardlink).set(Cap::kMknod);
  caps_.set(Cap::kStableHandles);  // GFIDs are cluster-persistent (06 §6.6)
  caps_.set(Cap::kNativeAccess);   // glfs_h_access: bricks decide
  caps_.set(Cap::kSparseOps).set(Cap::kCopyRange);
  if (cfg_.jukebox) caps_.set(Cap::kJukebox);
  if (cfg_.native_locks) caps_.set(Cap::kByteLocks);
  // Gluster's own wire limit is 1 MiB per fop; prefer that for read/write.
  limits_.max_read = limits_.pref_read = 1u << 20;
  limits_.max_write = limits_.pref_write = 1u << 20;
  fd_cache_ = std::make_unique<FdCache>(*this, cfg_.fd_cache);
  obj_cache_ = std::make_unique<ObjCache>(cfg_.fd_cache);
  if (cfg_.native_locks) locks_ = std::make_unique<GlusterLockMgr>(*this);
}

Result<std::unique_ptr<GlusterBackend>> GlusterBackend::create(
    Config cfg, std::shared_ptr<const gfapi::Api> api) {
  if (cfg.volume.empty() || cfg.fsid == 0) return Err(errno_from(EINVAL));
  if (cfg.subdir.empty() || cfg.subdir.front() != '/') return Err(errno_from(EINVAL));
  if (cfg.transport != "tcp" && cfg.transport != "rdma" && cfg.transport != "unix")
    return Err(errno_from(EINVAL));
  if (cfg.servers.empty()) cfg.servers.push_back({"localhost", 24007});
  for (const auto& s : cfg.servers)
    if (s.host.empty() || s.port <= 0 || s.port > 65535) return Err(errno_from(EINVAL));
  if (api && !gfapi::complete(*api)) return Err(errno_from(EINVAL));
  return std::unique_ptr<GlusterBackend>(new GlusterBackend(std::move(cfg), std::move(api)));
}

GlusterBackend::~GlusterBackend() {
  // stop() normally ran; this is the belt for a backend that never started or whose
  // stop() was skipped (tests).  Order: locks/fds/objects before glfs_fini.
  if (locks_) locks_->close_all();
  fd_cache_.reset();
  obj_cache_.reset();
  root_.reset();
  if (fs_) api_->glfs_fini(fs_);
}

std::optional<LockMgrRef> GlusterBackend::native_locks() {
  if (!locks_) return std::nullopt;
  return LockMgrRef(*locks_);
}

Errno GlusterBackend::map_errno(int e) const {
  switch (e) {
    case ENOTCONN:
    case ETIMEDOUT:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EHOSTDOWN:
      // Bricks away (reconnect / quorum loss / self-heal window): the volume is
      // expected back, so ask the client to retry rather than fail its IO (kJukebox →
      // v3 JUKEBOX / v4 DELAY).  Off by config → EIO.
      if (cfg_.jukebox) {
        jukebox_.fetch_add(1, std::memory_order_relaxed);
        return Errno::kJukebox;
      }
      return errno_from(EIO);
    case 0: return errno_from(EIO);  // libgfapi failed without setting errno
    default: return errno_from(e);
  }
}

Result<GlusterBackend::Gfid> GlusterBackend::gfid_from_oid(const ObjId& oid) {
  auto bytes = oid.view();
  if (bytes.size() != kObjIdLen || bytes[0] != kGfidHandle) return Err(errno_from(ESTALE));
  Gfid out{};
  std::memcpy(out.data(), bytes.data() + 1, out.size());
  return out;
}

ObjId GlusterBackend::oid_from_gfid(const Gfid& gfid) {
  std::array<std::byte, kObjIdLen> encoded{};
  encoded[0] = kGfidHandle;
  std::memcpy(encoded.data() + 1, gfid.data(), gfid.size());
  return *ObjId::from(encoded);
}

Result<ObjId> GlusterBackend::oid_of(glfs_object* obj) const {
  Gfid gfid{};
  ssize_t n = api_->glfs_h_extract_handle(obj, gfid.data(), static_cast<int>(gfid.size()));
  if (n != static_cast<ssize_t>(gfid.size())) return Err(map_errno(errno));
  return oid_from_gfid(gfid);
}

Result<Attr> GlusterBackend::attr_from_stat(const struct stat& st) const {
  Attr a;
  a.type = mode_type(st.st_mode);
  a.mode = st.st_mode & 07777;
  a.nlink = st.st_nlink;
  a.uid = st.st_uid;
  a.gid = st.st_gid;
  a.size = st.st_size;
  a.used = static_cast<uint64_t>(st.st_blocks) * 512;
  a.rdev = DevT{static_cast<uint32_t>(major(st.st_rdev)),
                static_cast<uint32_t>(minor(st.st_rdev))};
  a.fileid = st.st_ino;
  a.atime = convert_time(st.st_atim);
  a.mtime = convert_time(st.st_mtim);
  a.ctime = convert_time(st.st_ctim);
  // No native change counter in gluster (06 §6.6) → the 05 §5.6 synthesis.
  a.change = static_cast<uint64_t>(std::max<int64_t>(a.ctime.sec, 0)) * 1000000000ull +
             a.ctime.nsec;
  return a;
}

Result<Attr> GlusterBackend::stat_sync(glfs_object* obj) const {
  struct stat st {};
  if (api_->glfs_h_stat(fs_, obj, &st) < 0) return Err(map_errno(errno));
  return attr_from_stat(st);
}

Result<GlusterBackend::ObjRef> GlusterBackend::handle_from_oid_sync(const ObjId& oid,
                                                                    struct stat* st) {
  auto gfid = gfid_from_oid(oid);
  if (!gfid) return Err(gfid.error());
  struct stat local {};
  glfs_object* obj =
      api_->glfs_h_create_from_handle(fs_, gfid->data(), static_cast<int>(gfid->size()),
                                      st ? st : &local);
  if (!obj) {
    int e = errno;
    if (e == ENOENT || e == ESTALE || e == EINVAL) return Err(errno_from(ESTALE));
    return Err(map_errno(e));
  }
  return std::make_shared<ObjHandle>(api_.get(), obj);
}

ObjPtr GlusterBackend::wrap(ObjRef ref, const ObjId& oid, FType type) {
  return std::static_pointer_cast<Object>(
      std::shared_ptr<GlusterObject>(new GlusterObject(*this, std::move(ref), oid, type)));
}

Result<ObjPtr> GlusterBackend::wrap_new(glfs_object* obj, const struct stat& st) {
  auto ref = std::make_shared<ObjHandle>(api_.get(), obj);  // adopts (closes on error)
  auto oid = oid_of(obj);
  if (!oid) return Err(oid.error());
  auto entry = obj_cache_->insert(*oid, std::make_shared<ObjCache::Entry>(
                                            *oid, ref, mode_type(st.st_mode)));
  return wrap(entry->ref, *oid, entry->type);
}

bool GlusterBackend::valid_name(std::string_view name, bool allow_dotdot) {
  if (name.empty() || name.find('/') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos)
    return false;
  if (!allow_dotdot && (name == "." || name == "..")) return false;
  return true;
}

rt::Task<Result<void>> GlusterBackend::start() {
  if (fs_) co_return Result<void>{};
  if (!api_) {
    std::string detail;
    auto loaded = gfapi::load_system_api(&detail);
    if (!loaded) {
      LNFS_ERROR("gluster volume {}: libgfapi unavailable: {}", cfg_.volume, detail);
      co_return Err(loaded.error());
    }
    api_ = *loaded;
    fd_cache_ = std::make_unique<FdCache>(*this, cfg_.fd_cache);
  }
  // glfs_init blocks until the volfile is fetched and the graph is up (or fails):
  // strictly offload material.
  auto cfg = cfg_;
  auto api = api_;
  struct Started {
    glfs* fs;
    glfs_object* root;
    struct stat st;
    std::string volid;
  };
  auto started = co_await rt::offload([cfg, api]() -> Result<Started> {
    glfs* fs = api->glfs_new(cfg.volume.c_str());
    if (!fs) return Err(errno_from(errno ? errno : ENOMEM));
    auto fail = [&](int e) {
      api->glfs_fini(fs);
      return Err(errno_from(e ? e : EIO));
    };
    for (const auto& s : cfg.servers)
      if (api->glfs_set_volfile_server(fs, cfg.transport.c_str(), s.host.c_str(), s.port) < 0)
        return fail(errno);
    if (!cfg.log_file.empty() &&
        api->glfs_set_logging(fs, cfg.log_file.c_str(), cfg.log_level) < 0)
      return fail(errno);
    if (api->glfs_init(fs) < 0) return fail(errno);
    struct stat st {};
    glfs_object* root = api->glfs_h_lookupat(fs, nullptr, cfg.subdir.c_str(), &st, 1);
    if (!root) return fail(errno);
    if (!S_ISDIR(st.st_mode)) {
      api->glfs_h_close(root);
      return fail(ENOTDIR);
    }
    char volid[64] = {};
    std::string id;
    if (api->glfs_get_volumeid(fs, volid, sizeof volid) > 0) {
      static const char* hex = "0123456789abcdef";
      for (int i = 0; i < 16; ++i) {
        auto b = static_cast<unsigned char>(volid[i]);
        id.push_back(hex[b >> 4]);
        id.push_back(hex[b & 15]);
      }
    }
    return Started{fs, root, st, id};
  }, rt::OffloadClass::kHeavy);
  if (!started) {
    LNFS_ERROR("gluster volume {} ({}): connect failed: {}", cfg_.volume, cfg_.subdir,
               errno_name(started.error()));
    co_return Err(started.error());
  }
  fs_ = started->fs;
  root_ = std::make_shared<ObjHandle>(api_.get(), started->root);
  auto oid = oid_of(started->root);
  if (!oid) {
    root_.reset();
    api_->glfs_fini(fs_);
    fs_ = nullptr;
    co_return Err(oid.error());
  }
  root_oid_ = *oid;
  volume_id_ = started->volid;
  obj_cache_->insert(root_oid_,
                     std::make_shared<ObjCache::Entry>(root_oid_, root_, FType::kDir));
  LNFS_INFO("gluster volume {} ({}) up: volume-id {} native-locks={} jukebox={}",
            cfg_.volume, cfg_.subdir, volume_id_.empty() ? "?" : volume_id_,
            cfg_.native_locks, cfg_.jukebox);
  co_return Result<void>{};
}

rt::Task<Result<void>> GlusterBackend::stop() {
  if (!fs_) co_return Result<void>{};
  if (locks_) locks_->close_all();
  fd_cache_->clear();
  obj_cache_->clear();
  root_.reset();
  glfs* fs = fs_;
  fs_ = nullptr;
  auto api = api_;
  co_await rt::offload([api, fs] { api->glfs_fini(fs); }, rt::OffloadClass::kHeavy);
  co_return Result<void>{};
}

rt::Task<Result<ObjPtr>> GlusterBackend::root() {
  if (!fs_) co_return Err(errno_from(ENOTCONN));
  co_return wrap(root_, root_oid_, FType::kDir);
}

rt::Task<Result<ObjPtr>> GlusterBackend::resolve(const ObjId& oid) {
  if (!fs_) co_return Err(errno_from(ENOTCONN));
  if (auto hit = obj_cache_->find(oid)) co_return wrap(hit->ref, oid, hit->type);
  auto got = co_await rt::offload([this, oid]() -> Result<std::pair<ObjRef, FType>> {
    struct stat st {};
    auto ref = handle_from_oid_sync(oid, &st);
    if (!ref) return Err(ref.error());
    return std::pair{*ref, mode_type(st.st_mode)};
  });
  if (!got) co_return Err(got.error());
  auto entry = obj_cache_->insert(
      oid, std::make_shared<ObjCache::Entry>(oid, got->first, got->second));
  co_return wrap(entry->ref, oid, entry->type);
}

rt::Task<Result<FsStats>> GlusterBackend::statfs() {
  if (!fs_) co_return Err(errno_from(ENOTCONN));
  auto root = root_;
  co_return co_await rt::offload([this, root]() -> Result<FsStats> {
    struct statvfs s {};
    if (api_->glfs_h_statfs(fs_, root->obj, &s) < 0) return Err(map_errno(errno));
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

GlusterBackend::Stats GlusterBackend::stats() const {
  Stats out;
  if (fd_cache_) fd_cache_->fill(out);
  if (obj_cache_) {
    out.obj_hits = obj_cache_->hits();
    out.obj_misses = obj_cache_->misses();
    out.obj_entries = obj_cache_->entries();
  }
  out.jukebox = jukebox_.load(std::memory_order_relaxed);
  if (locks_) out.lock_fds = locks_->fds();
  return out;
}

size_t GlusterBackend::flush_fd_cache() {
  return (fd_cache_ ? fd_cache_->flush() : 0) + (obj_cache_ ? obj_cache_->flush() : 0);
}

void GlusterBackend::poison(const ObjId& oid) {
  std::lock_guard lock(poison_mu_);
  poisoned_.insert(oid);
}

bool GlusterBackend::is_poisoned(const ObjId& oid) const {
  std::lock_guard lock(poison_mu_);
  return poisoned_.contains(oid);
}

size_t GlusterBackend::clear_poison() {
  std::lock_guard lock(poison_mu_);
  size_t n = poisoned_.size();
  poisoned_.clear();
  return n;
}

// ---- object: metadata -------------------------------------------------------

rt::Task<Result<Attr>> GlusterObject::getattr() {
  auto ref = ref_;
  co_return co_await rt::offload([this, ref] { return backend_.stat_sync(ref->obj); });
}

rt::Task<Result<AccessMask>> GlusterObject::access(const Cred& cred, AccessMask want) {
  // Bricks authorize (posix-acl xlator: mode bits + POSIX ACLs) under the caller's
  // fsuid/fsgid/groups.  One glfs_h_access per distinct POSIX mode the request needs
  // (at most three round trips, usually one).
  OwnedCred owned(cred);
  auto ref = ref_;
  bool dir = type() == FType::kDir;
  co_return co_await rt::offload([this, ref, owned, want, dir]() -> Result<AccessMask> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, owned.uid, owned.gid, owned.groups());
    auto probe = [&](int mode) -> Result<bool> {
      if (api.glfs_h_access(backend_.fs_, ref->obj, mode) == 0) return true;
      if (errno == EACCES || errno == EPERM || errno == EROFS) return false;
      return Err(backend_.map_errno(errno));
    };
    AccessMask allowed;
    if (want.has(Access::kRead)) {
      auto r = probe(R_OK);
      if (!r) return Err(r.error());
      if (*r) allowed.set(Access::kRead);
    }
    if (want.has(Access::kModify) || want.has(Access::kExtend) || want.has(Access::kDelete)) {
      auto w = probe(W_OK);
      if (!w) return Err(w.error());
      if (*w) {
        if (want.has(Access::kModify)) allowed.set(Access::kModify);
        if (want.has(Access::kExtend)) allowed.set(Access::kExtend);
        if (want.has(Access::kDelete) && dir) allowed.set(Access::kDelete);
      }
    }
    if (want.has(Access::kLookup) || want.has(Access::kExecute)) {
      auto x = probe(X_OK);
      if (!x) return Err(x.error());
      if (*x) {
        if (want.has(Access::kLookup) && dir) allowed.set(Access::kLookup);
        if (want.has(Access::kExecute) && !dir) allowed.set(Access::kExecute);
      }
    }
    return allowed;
  });
}

rt::Task<Result<void>> GlusterObject::require_dir(const Cred&) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  co_return Result<void>{};  // the bricks enforce write permission on the directory
}

rt::Task<Result<ObjPtr>> GlusterObject::lookup(const Cred& cred, std::string_view name) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (!GlusterBackend::valid_name(name, true)) co_return Err(errno_from(EINVAL));
  if (name == "." || (name == ".." && id() == backend_.root_oid_))
    co_return backend_.wrap(ref_, id(), FType::kDir);
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, owned, oc]() -> Result<ObjPtr> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    struct stat st {};
    glfs_object* child = api.glfs_h_lookupat(backend_.fs_, ref->obj, owned.c_str(), &st, 0);
    if (!child) return Err(backend_.map_errno(errno));
    return backend_.wrap_new(child, st);
  });
}

rt::Task<Result<Attr>> GlusterObject::setattr(const Cred& cred, const SetAttr& s) {
  OwnedCred oc(cred);
  auto ref = ref_;
  auto applied = co_await rt::offload([this, ref, oc, s]() -> Result<void> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    if (s.size) {
      if (type() == FType::kDir) return Err(errno_from(EISDIR));
      if (type() != FType::kReg) return Err(errno_from(EINVAL));
      if (api.glfs_h_truncate(backend_.fs_, ref->obj, static_cast<off_t>(*s.size)) < 0)
        return Err(backend_.map_errno(errno));
    }
    struct stat sb {};
    int valid = 0;
    if (s.mode) {
      sb.st_mode = *s.mode & 07777;
      valid |= gfapi::kSetMode;
    }
    if (s.uid) {
      sb.st_uid = *s.uid;
      valid |= gfapi::kSetUid;
    }
    if (s.gid) {
      sb.st_gid = *s.gid;
      valid |= gfapi::kSetGid;
    }
    if (s.atime_how != SetAttr::TimeHow::kOmit || s.mtime_how != SetAttr::TimeHow::kOmit) {
      // libgfapi has no UTIME_NOW: "server time" is sampled here (the gateway clock
      // is the storage clock as far as the client can tell; time_delta says 1ns).
      timespec now{};
      clock_gettime(CLOCK_REALTIME, &now);
      if (s.atime_how != SetAttr::TimeHow::kOmit) {
        sb.st_atim = s.atime_how == SetAttr::TimeHow::kServer ? now : to_timespec(s.atime);
        valid |= gfapi::kSetAtime;
      }
      if (s.mtime_how != SetAttr::TimeHow::kOmit) {
        sb.st_mtim = s.mtime_how == SetAttr::TimeHow::kServer ? now : to_timespec(s.mtime);
        valid |= gfapi::kSetMtime;
      }
    }
    if (valid && api.glfs_h_setattrs(backend_.fs_, ref->obj, &sb, valid) < 0 &&
        !(type() == FType::kLnk && (valid & gfapi::kSetMode)))
      return Err(backend_.map_errno(errno));
    return {};
  });
  if (!applied) co_return Err(applied.error());
  co_return co_await getattr();
}

// ---- object: creation family ------------------------------------------------

Result<Created> GlusterObject::created_sync(glfs_object* child, const struct stat& st,
                                            const Cred& cred,
                                            std::optional<uint32_t> want_mode) {
  const auto& api = *backend_.api_;
  struct stat cur = st;
  // Gluster applies the brick's umask on creation; a CREATE/MKDIR that named a mode
  // must land exactly that mode (the local backend does the same fchmod).
  if (want_mode && (cur.st_mode & 07777) != (*want_mode & 07777)) {
    struct stat sb {};
    sb.st_mode = *want_mode & 07777;
    if (api.glfs_h_setattrs(backend_.fs_, child, &sb, gfapi::kSetMode) == 0)
      cur.st_mode = (cur.st_mode & ~07777u) | (*want_mode & 07777);
  }
  (void)cred;
  auto attr = backend_.attr_from_stat(cur);
  auto obj = backend_.wrap_new(child, cur);  // adopts child (closed on failure)
  if (!obj) return Err(obj.error());
  return Created{std::move(*obj), *attr};
}

rt::Task<Result<Created>> GlusterObject::create(const Cred& cred, std::string_view name,
                                                const SetAttr& attrs, ExclVerf* verf) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
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
        ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
        mode_t mode = exclusive ? 0 : (attrs.mode.value_or(0644) & 07777);
        struct stat st {};
        glfs_object* child = api.glfs_h_creat(backend_.fs_, ref->obj, owned.c_str(),
                                              O_CREAT | O_EXCL | O_RDWR, mode, &st);
        if (!child && errno == EEXIST && exclusive) {
          // Retransmitted EXCLUSIVE create: the verifier lives in atime/mtime.
          child = api.glfs_h_lookupat(backend_.fs_, ref->obj, owned.c_str(), &st, 0);
          if (child && S_ISREG(st.st_mode)) {
            int64_t lo = 0, hi = 0;
            verf_split(verf_copy, lo, hi);
            if (st.st_atim.tv_sec == lo && st.st_mtim.tv_sec == hi)
              return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, std::nullopt);
          }
          if (child) api.glfs_h_close(child);
          return Err(errno_from(EEXIST));
        }
        if (!child) return Err(backend_.map_errno(errno));
        if (exclusive) {
          int64_t lo = 0, hi = 0;
          verf_split(verf_copy, lo, hi);
          struct stat sb {};
          sb.st_atim = {static_cast<time_t>(lo), 0};
          sb.st_mtim = {static_cast<time_t>(hi), 0};
          if (api.glfs_h_setattrs(backend_.fs_, child, &sb,
                                  gfapi::kSetAtime | gfapi::kSetMtime) < 0) {
            int e = errno;
            api.glfs_h_close(child);
            (void)api.glfs_h_unlink(backend_.fs_, ref->obj, owned.c_str());
            return Err(backend_.map_errno(e));
          }
          st.st_atim = sb.st_atim;
          st.st_mtim = sb.st_mtim;
          return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, std::nullopt);
        }
        if (attrs.size &&
            api.glfs_h_truncate(backend_.fs_, child, static_cast<off_t>(*attrs.size)) < 0) {
          int e = errno;
          api.glfs_h_close(child);
          return Err(backend_.map_errno(e));
        }
        if (attrs.size) st.st_size = static_cast<off_t>(*attrs.size);
        return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, mode);
      });
}

rt::Task<Result<Created>> GlusterObject::mkdir(const Cred& cred, std::string_view name,
                                               const SetAttr& attrs) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  mode_t mode = attrs.mode.value_or(0755) & 07777;
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned, mode]() -> Result<Created> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    struct stat st {};
    glfs_object* child = api.glfs_h_mkdir(backend_.fs_, ref->obj, owned.c_str(), mode, &st);
    if (!child) return Err(backend_.map_errno(errno));
    return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, mode);
  });
}

rt::Task<Result<Created>> GlusterObject::symlink(const Cred& cred, std::string_view name,
                                                 std::string_view target, const SetAttr&) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  if (target.empty() || target.find('\0') != std::string_view::npos)
    co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name), owned_target(target);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned, owned_target]() -> Result<Created> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    struct stat st {};
    glfs_object* child = api.glfs_h_symlink(backend_.fs_, ref->obj, owned.c_str(),
                                            owned_target.c_str(), &st);
    if (!child) return Err(backend_.map_errno(errno));
    return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, std::nullopt);
  });
}

rt::Task<Result<Created>> GlusterObject::mknod(const Cred& cred, std::string_view name,
                                               FType ftype, DevT dev, const SetAttr& attrs) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
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
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    struct stat st {};
    glfs_object* child =
        api.glfs_h_mknod(backend_.fs_, ref->obj, owned.c_str(), mode, rdev, &st);
    if (!child) return Err(backend_.map_errno(errno));
    return created_sync(child, st, Cred{oc.uid, oc.gid, oc.groups()}, mode & 07777);
  });
}

rt::Task<Result<void>> GlusterObject::unlink(const Cred& cred, std::string_view name) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    // glfs_h_unlink removes directories too; REMOVE on a directory must say EISDIR
    // (the local backend gets that from unlinkat), so type-check first.
    struct stat st {};
    glfs_object* child = api.glfs_h_lookupat(backend_.fs_, ref->obj, owned.c_str(), &st, 0);
    if (!child) return Err(backend_.map_errno(errno));
    bool is_dir = S_ISDIR(st.st_mode);
    auto oid = backend_.oid_of(child);
    api.glfs_h_close(child);
    if (is_dir) return Err(errno_from(EISDIR));
    if (api.glfs_h_unlink(backend_.fs_, ref->obj, owned.c_str()) < 0)
      return Err(backend_.map_errno(errno));
    if (oid) backend_.fd_cache_->drop(*oid);  // no reason to keep a glfd on a dead file
    return {};
  });
}

rt::Task<Result<void>> GlusterObject::rmdir(const Cred& cred, std::string_view name) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto ref = ref_;
  co_return co_await rt::offload([this, ref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    struct stat st {};
    glfs_object* child = api.glfs_h_lookupat(backend_.fs_, ref->obj, owned.c_str(), &st, 0);
    if (!child) return Err(backend_.map_errno(errno));
    bool is_dir = S_ISDIR(st.st_mode);
    api.glfs_h_close(child);
    if (!is_dir) return Err(errno_from(ENOTDIR));
    if (api.glfs_h_unlink(backend_.fs_, ref->obj, owned.c_str()) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  });
}

rt::Task<Result<void>> GlusterObject::rename(const Cred& cred, std::string_view from,
                                             Object& dst_dir, std::string_view to) {
  if (!GlusterBackend::valid_name(from) || !GlusterBackend::valid_name(to))
    co_return Err(errno_from(EINVAL));
  auto* dst = dynamic_cast<GlusterObject*>(&dst_dir);
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
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    if (api.glfs_h_rename(backend_.fs_, sref->obj, owned_from.c_str(), dref->obj,
                          owned_to.c_str()) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  });
}

rt::Task<Result<void>> GlusterObject::link(const Cred& cred, Object& file,
                                           std::string_view name) {
  if (!GlusterBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto* target = dynamic_cast<GlusterObject*>(&file);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (target->type() == FType::kDir) co_return Err(errno_from(EPERM));
  auto dir_ok = co_await require_dir(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  OwnedCred oc(cred);
  auto dref = ref_, tref = target->ref_;
  co_return co_await rt::offload([this, dref, tref, oc, owned]() -> Result<void> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    if (api.glfs_h_link(backend_.fs_, tref->obj, dref->obj, owned.c_str()) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  });
}

rt::Task<Result<DirPage>> GlusterObject::readdir(const Cred& cred, uint64_t cookie,
                                                 uint32_t max_entries) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (max_entries == 0) co_return DirPage{};
  OwnedCred oc(cred);
  auto ref = ref_;
  bool enrich = backend_.cfg_.enrich_readdir;
  co_return co_await rt::offload([this, ref, oc, cookie, max_entries, enrich]() -> Result<DirPage> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    // One directory glfd per page: libgfapi keeps the offset in the glfd and every
    // page is an independent request, so a shared descriptor would need the same
    // serialization the local backend's dents mutex provides — at cluster latency the
    // opendir/closedir pair is the cheaper price.
    glfs_fd* dir = api.glfs_h_opendir(backend_.fs_, ref->obj);
    if (!dir) return Err(backend_.map_errno(errno));
    struct Closer {
      const gfapi::Api& api;
      glfs_fd* fd;
      ~Closer() { api.glfs_closedir(fd); }
    } closer{api, dir};
    if (cookie != 0) api.glfs_seekdir(dir, static_cast<long>(cookie));
    uint32_t flags = enrich ? (gfapi::kXreaddirpStat | gfapi::kXreaddirpHandle) : 0;
    DirPage page;
    while (page.ents.size() < max_entries) {
      struct dirent ext {};
      struct dirent* res = nullptr;
      glfs_xreaddirp_stat* xstat = nullptr;
      errno = 0;
      int rc = api.glfs_xreaddirplus_r(dir, flags, &xstat, &ext, &res);
      if (rc < 0) {
        if (xstat) api.glfs_free(xstat);
        return Err(backend_.map_errno(errno));
      }
      if (!res) {
        if (xstat) api.glfs_free(xstat);
        page.eof = true;
        break;
      }
      std::string_view name(res->d_name);
      if (name == "." || name == "..") {
        if (xstat) api.glfs_free(xstat);
        continue;
      }
      DirPage::Ent out{.name = std::string(name),
                       .cookie = static_cast<uint64_t>(res->d_off),
                       .fileid = res->d_ino,
                       .attr = std::nullopt,
                       .oid = std::nullopt};
      if (xstat) {
        if (rc & gfapi::kXreaddirpStat) {
          if (struct stat* st = api.glfs_xreaddirplus_get_stat(xstat)) {
            auto attr = backend_.attr_from_stat(*st);
            if (attr) out.attr = *attr;
          }
        }
        if (rc & gfapi::kXreaddirpHandle) {
          // The object is owned by xstat (freed with it); only its GFID is needed.
          if (glfs_object* child = api.glfs_xreaddirplus_get_object(xstat)) {
            auto oid = backend_.oid_of(child);
            if (oid) out.oid = *oid;
          }
        }
        api.glfs_free(xstat);
      }
      page.ents.push_back(std::move(out));
    }
    return page;
  });
}

rt::Task<Result<std::string>> GlusterObject::readlink() {
  if (type() != FType::kLnk) co_return Err(errno_from(EINVAL));
  auto ref = ref_;
  co_return co_await rt::offload([this, ref]() -> Result<std::string> {
    std::vector<char> buf(4096);
    int n = backend_.api_->glfs_h_readlink(backend_.fs_, ref->obj, buf.data(), buf.size());
    if (n < 0) return Err(backend_.map_errno(errno));
    if (static_cast<size_t>(n) >= buf.size()) return Err(errno_from(ENAMETOOLONG));
    return std::string(buf.data(), static_cast<size_t>(n));
  });
}

// ---- object: IO ---------------------------------------------------------------

rt::Task<Result<void>> GlusterObject::io_gate(const Cred& cred, bool write) {
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

rt::Task<Result<OpenPtr>> GlusterObject::open(const Cred& cred, OpenFlags flags) {
  // kNativeAccess contract: the bricks authorize this open under the caller's
  // identity — no EOPNOTSUPP degrade, an EACCES here is the OPEN's answer.
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  bool writable = flags.has(OpenFlag::kWrite);
  int oflags = writable ? O_RDWR : O_RDONLY;
  if (flags.has(OpenFlag::kTruncate) && writable) oflags |= O_TRUNC;
  OwnedCred oc(cred);
  auto ref = ref_;
  auto opened = co_await rt::offload([this, ref, oc, oflags]() -> Result<glfs_fd*> {
    const auto& api = *backend_.api_;
    ScopedIds ids(api, oc.uid, oc.gid, oc.groups());
    glfs_fd* fd = api.glfs_h_open(backend_.fs_, ref->obj, oflags);
    if (!fd) return Err(backend_.map_errno(errno));
    return fd;
  });
  if (!opened) co_return Err(opened.error());
  co_return OpenPtr(std::make_shared<GlusterOpenState>(backend_.api_.get(), *opened, writable));
}

namespace {
rt::Task<void> maybe_slow_io() {
  if (fault::take(fault::Kind::kSlowIo))
    co_await rt::sleep_for(std::chrono::milliseconds(fault::slow_ms()));
}
}  // namespace

rt::Task<Result<uint32_t>> GlusterObject::read(OpenCtx ctx, uint64_t off,
                                               std::span<std::byte> out, bool& eof) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  glfs_fd* fd = nullptr;
  GlusterBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx)) {
    fd = os->fd();
  } else {
    auto gate = co_await io_gate(ctx.cred, false);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fd = ref->fd;
  }
  co_await maybe_slow_io();
  if (fault::take(fault::Kind::kReadEio)) co_return Err(errno_from(EIO));
  if (fault::take(fault::Kind::kJukebox)) co_return Err(Errno::kJukebox);
  auto n = co_await rt::offload([this, fd, off, out]() -> Result<uint32_t> {
    ssize_t r = backend_.api_->glfs_pread(fd, out.data(), out.size(), static_cast<off_t>(off),
                                          0, nullptr);
    if (r < 0) return Err(backend_.map_errno(errno));
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

rt::Task<Result<uint32_t>> GlusterObject::write(OpenCtx ctx, uint64_t off,
                                                std::span<const std::byte> in,
                                                Stability stability) {
  iovec v{const_cast<std::byte*>(in.data()), in.size()};
  co_return co_await write(ctx, off, std::span<const iovec>(&v, 1), stability);
}

rt::Task<Result<uint32_t>> GlusterObject::write(OpenCtx ctx, uint64_t off,
                                                std::span<const iovec> iov,
                                                Stability stability) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  glfs_fd* fd = nullptr;
  GlusterBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx); os && os->writable()) {
    fd = os->fd();
  } else {
    auto gate = co_await io_gate(ctx.cred, true);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, true);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fd = ref->fd;
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
  auto done = co_await rt::offload([this, fd, off, vec, total, stability, short_write,
                                    sync_fault]() mutable -> Result<uint32_t> {
    const auto& api = *backend_.api_;
    size_t done = 0, idx = 0;
    while (done < total) {
      ssize_t n;
      if (short_write) {  // fault: 1 byte, exercising the iovec advance below
        short_write = false;
        n = api.glfs_pwrite(fd, vec[idx].iov_base, 1, static_cast<off_t>(off + done), 0,
                            nullptr, nullptr);
      } else {
        n = api.glfs_pwritev(fd, vec.data() + idx, static_cast<int>(vec.size() - idx),
                             static_cast<off_t>(off + done), 0);
      }
      if (n < 0) return Err(backend_.map_errno(errno));
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
      int rc = sync_fault ? -1
               : stability == Stability::kDataSync ? api.glfs_fdatasync(fd, nullptr, nullptr)
                                                   : api.glfs_fsync(fd, nullptr, nullptr);
      if (rc < 0) {
        backend_.poison(id());
        return Err(sync_fault ? errno_from(EIO) : backend_.map_errno(errno));
      }
    }
    return static_cast<uint32_t>(done);
  }, stability == Stability::kUnstable ? rt::OffloadClass::kLight : rt::OffloadClass::kHeavy);
  co_return done;
}

rt::Task<Result<void>> GlusterObject::commit(OpenCtx ctx, uint64_t, uint64_t) {
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  if (backend_.is_poisoned(id())) co_return Err(errno_from(EIO));  // sticky (06 §6.2)
  glfs_fd* fd = nullptr;
  GlusterBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx); os && os->writable()) {
    fd = os->fd();
  } else {
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fd = ref->fd;
  }
  bool sync_fault = fault::take(fault::Kind::kFsyncEio);
  co_return co_await rt::offload([this, fd, sync_fault]() -> Result<void> {
    int rc = sync_fault ? -1 : backend_.api_->glfs_fdatasync(fd, nullptr, nullptr);
    if (rc < 0) {
      backend_.poison(id());
      return Err(sync_fault ? errno_from(EIO) : backend_.map_errno(errno));
    }
    return {};
  }, rt::OffloadClass::kHeavy);
}

// ---- object: v4.2 -------------------------------------------------------------

rt::Task<Result<uint64_t>> GlusterObject::seek(OpenCtx ctx, uint64_t off, SeekWhat what) {
  glfs_fd* fd = nullptr;
  GlusterBackend::FdCache::Ref ref;
  if (auto* os = open_state(ctx)) {
    fd = os->fd();
  } else {
    auto gate = co_await io_gate(ctx.cred, false);
    if (!gate) co_return Err(gate.error());
    auto got = co_await backend_.fd_cache_->acquire(*this, false);
    if (!got) co_return Err(got.error());
    ref = std::move(*got);
    fd = ref->fd;
  }
  co_return co_await rt::offload([this, fd, off, what]() -> Result<uint64_t> {
    off_t r = backend_.api_->glfs_lseek(fd, static_cast<off_t>(off),
                                        what == SeekWhat::kData ? SEEK_DATA : SEEK_HOLE);
    if (r < 0) return Err(backend_.map_errno(errno));  // ENXIO past EOF / no data
    return static_cast<uint64_t>(r);
  });
}

rt::Task<Result<void>> GlusterObject::allocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(*this, true);
  if (!ref) co_return Err(ref.error());
  glfs_fd* fd = (*ref)->fd;
  co_return co_await rt::offload([this, fd, off, len]() -> Result<void> {
    if (backend_.api_->glfs_fallocate(fd, 0, static_cast<off_t>(off), len) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  }, rt::OffloadClass::kHeavy);
}

rt::Task<Result<void>> GlusterObject::deallocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(*this, true);
  if (!ref) co_return Err(ref.error());
  glfs_fd* fd = (*ref)->fd;
  co_return co_await rt::offload([this, fd, off, len]() -> Result<void> {
    if (backend_.api_->glfs_discard(fd, static_cast<off_t>(off), len) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  }, rt::OffloadClass::kHeavy);
}

rt::Task<Result<uint64_t>> GlusterObject::copy_range(OpenCtx sctx, Object& dst, OpenCtx dctx,
                                                     uint64_t src_off, uint64_t dst_off,
                                                     uint64_t len) {
  auto* target = dynamic_cast<GlusterObject*>(&dst);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto sgate = co_await io_gate(sctx.cred, false);
  if (!sgate) co_return Err(sgate.error());
  auto dgate = co_await target->io_gate(dctx.cred, true);
  if (!dgate) co_return Err(dgate.error());
  auto sref = co_await backend_.fd_cache_->acquire(*this, false);
  if (!sref) co_return Err(sref.error());
  auto dref = co_await backend_.fd_cache_->acquire(*target, true);
  if (!dref) co_return Err(dref.error());
  glfs_fd* sfd = (*sref)->fd;
  glfs_fd* dfd = (*dref)->fd;
  co_return co_await rt::offload([this, sfd, dfd, src_off, dst_off, len]() -> Result<uint64_t> {
    const auto& api = *backend_.api_;
    uint64_t want = len;
    if (want == 0) {  // to EOF
      struct stat st {};
      if (api.glfs_fstat(sfd, &st) < 0) return Err(backend_.map_errno(errno));
      if (static_cast<uint64_t>(st.st_size) <= src_off) return 0;
      want = static_cast<uint64_t>(st.st_size) - src_off;
    }
    uint64_t done = 0;
    bool native = true;
    std::vector<std::byte> buf;
    while (done < want) {
      off64_t in = static_cast<off64_t>(src_off + done);
      off64_t out = static_cast<off64_t>(dst_off + done);
      size_t chunk = static_cast<size_t>(std::min<uint64_t>(want - done, 1ull << 20));
      ssize_t n = -1;
      if (native) {
        n = api.glfs_copy_file_range(sfd, &in, dfd, &out, chunk, 0, nullptr, nullptr, nullptr);
        if (n < 0 && (errno == EXDEV || errno == EOPNOTSUPP || errno == ENOSYS ||
                      errno == EINVAL || errno == ENOTSUP)) {
          native = false;  // volume without the copy_file_range fop: pread/pwrite
          n = -1;
        } else if (n < 0) {
          return Err(backend_.map_errno(errno));
        }
      }
      if (!native) {
        if (buf.empty()) buf.resize(1u << 20);
        size_t rd = std::min(chunk, buf.size());
        ssize_t r = api.glfs_pread(sfd, buf.data(), rd, in, 0, nullptr);
        if (r < 0) return Err(backend_.map_errno(errno));
        if (r == 0) break;
        size_t w = 0;
        while (w < static_cast<size_t>(r)) {
          ssize_t k = api.glfs_pwrite(dfd, buf.data() + w, static_cast<size_t>(r) - w, out + w,
                                      0, nullptr, nullptr);
          if (k < 0) return Err(backend_.map_errno(errno));
          if (k == 0) return Err(errno_from(EIO));
          w += static_cast<size_t>(k);
        }
        n = r;
      }
      if (n == 0) break;
      done += static_cast<uint64_t>(n);
    }
    return done;
  }, rt::OffloadClass::kHeavy);
}

// ---- native byte-range locks ---------------------------------------------------

size_t GlusterLockMgr::KeyHash::operator()(const Key& k) const noexcept {
  return ObjIdHash{}(k.oid) ^ (std::hash<std::string>{}(k.owner) << 1);
}

GlusterLockMgr::~GlusterLockMgr() { close_all(); }

void GlusterLockMgr::close_all() {
  std::lock_guard lock(mu_);
  for (auto& [key, fd] : fds_) backend_.api_->glfs_close(fd);
  fds_.clear();
}

size_t GlusterLockMgr::fds() const {
  std::lock_guard lock(const_cast<std::mutex&>(mu_));
  return fds_.size();
}

struct flock GlusterLockMgr::make_flock(LockRange range, short type) {
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

// Runs on an offload worker.  Lock descriptors are opened as the gateway (uid 0):
// the state layer already checked the OPEN's access mode covers the lock type
// (RFC 8881 §18.10.3), and posix-locks does not re-check permissions per fcntl.
Result<glfs_fd*> GlusterLockMgr::fd_for(GlusterObject& obj, const LockOwnerId& owner,
                                        bool create) {
  Key key{obj.id(), std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
  {
    std::lock_guard lock(mu_);
    auto it = fds_.find(key);
    if (it != fds_.end()) return it->second;
  }
  if (!create) return Err(errno_from(ENOENT));
  const auto& api = *backend_.api_;
  ScopedIds ids(api, 0, 0, {});
  glfs_fd* fd = api.glfs_h_open(backend_.fs_, obj.handle(), O_RDWR);
  if (!fd && (errno == EACCES || errno == EROFS || errno == EPERM))
    fd = api.glfs_h_open(backend_.fs_, obj.handle(), O_RDONLY);
  if (!fd) return Err(backend_.map_errno(errno));
  if (api.glfs_fd_set_lkowner(fd, const_cast<std::byte*>(owner.bytes.data()), owner.len) < 0) {
    int e = errno;
    api.glfs_close(fd);
    return Err(backend_.map_errno(e));
  }
  std::lock_guard lock(mu_);
  auto [it, inserted] = fds_.emplace(key, fd);
  if (!inserted) {  // lost a race: keep the winner
    api.glfs_close(fd);
    return it->second;
  }
  return fd;
}

rt::Task<Result<void>> GlusterLockMgr::lock(Object& object, const LockOwnerId& owner,
                                            LockRange range, bool exclusive, bool wait) {
  auto* obj = dynamic_cast<GlusterObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner, range, exclusive, wait]() -> Result<void> {
    auto fd = fd_for(*obj, owner, true);
    if (!fd) return Err(fd.error());
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    // The state layer never blocks a request on a lock (RFC 8881 §18.10: clients
    // poll / get CB_NOTIFY_LOCK), so `wait` is accepted but not honoured.
    (void)wait;
    if (backend_.api_->glfs_posix_lock(*fd, F_SETLK, &fl) < 0) {
      int e = errno;
      if (e == EAGAIN || e == EACCES) return Err(errno_from(EAGAIN));  // conflict
      return Err(backend_.map_errno(e));
    }
    return {};
  });
}

rt::Task<Result<void>> GlusterLockMgr::unlock(Object& object, const LockOwnerId& owner,
                                              LockRange range) {
  auto* obj = dynamic_cast<GlusterObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner, range]() -> Result<void> {
    auto fd = fd_for(*obj, owner, false);
    if (!fd) return {};  // nothing held by this owner on this file: unlocking is idempotent
    struct flock fl = make_flock(range, F_UNLCK);
    if (backend_.api_->glfs_posix_lock(*fd, F_SETLK, &fl) < 0)
      return Err(backend_.map_errno(errno));
    return {};
  });
}

rt::Task<Result<std::optional<LockConflict>>> GlusterLockMgr::test(Object& object,
                                                                   LockRange range,
                                                                   bool exclusive) {
  auto* obj = dynamic_cast<GlusterObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, range, exclusive]() -> Result<std::optional<LockConflict>> {
    // A probe descriptor with an owner no client can have (empty lk-owner): F_GETLK
    // reports any conflicting holder, local or on another gateway.
    const auto& api = *backend_.api_;
    ScopedIds ids(api, 0, 0, {});
    glfs_fd* fd = api.glfs_h_open(backend_.fs_, obj->handle(), O_RDONLY);
    if (!fd) return Err(backend_.map_errno(errno));
    struct Closer {
      const gfapi::Api& api;
      glfs_fd* fd;
      ~Closer() { api.glfs_close(fd); }
    } closer{api, fd};
    static unsigned char probe_owner[8] = {'l', 'n', 'f', 's', 'p', 'r', 'o', 'b'};
    (void)api.glfs_fd_set_lkowner(fd, probe_owner, sizeof probe_owner);
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    if (api.glfs_posix_lock(fd, F_GETLK, &fl) < 0) return Err(backend_.map_errno(errno));
    if (fl.l_type == F_UNLCK) return std::optional<LockConflict>{};
    LockConflict c;
    c.exclusive = fl.l_type == F_WRLCK;
    c.range.offset = static_cast<uint64_t>(fl.l_start);
    c.range.length = fl.l_len == 0 ? UINT64_MAX : static_cast<uint64_t>(fl.l_len);
    // The holder's lk-owner is not reported by F_GETLK; an empty owner tells the
    // state layer "someone else, possibly on another gateway".
    return std::optional<LockConflict>(c);
  });
}

rt::Task<Result<void>> GlusterLockMgr::release(Object& object, const LockOwnerId& owner) {
  auto* obj = dynamic_cast<GlusterObject*>(&object);
  if (!obj || &obj->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto keep = obj->ref_;
  co_return co_await rt::offload([this, obj, owner]() -> Result<void> {
    Key key{obj->id(),
            std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
    glfs_fd* fd = nullptr;
    {
      std::lock_guard lock(mu_);
      auto it = fds_.find(key);
      if (it == fds_.end()) return {};
      fd = it->second;
      fds_.erase(it);
    }
    // Closing the descriptor drops every lock it holds (POSIX/posix-locks semantics);
    // the explicit full-range unlock first keeps the bricks' view exact even where a
    // close is delayed by a pending fop.
    struct flock fl = make_flock({0, UINT64_MAX}, F_UNLCK);
    (void)backend_.api_->glfs_posix_lock(fd, F_SETLK, &fl);
    backend_.api_->glfs_close(fd);
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

// "host1,host2:24008,[::1]:24007" → servers
bool parse_servers(const std::string& value, std::vector<GlusterBackend::Server>& out) {
  size_t pos = 0;
  while (pos <= value.size()) {
    size_t comma = value.find(',', pos);
    std::string item = value.substr(pos, comma == std::string::npos ? std::string::npos
                                                                     : comma - pos);
    pos = comma == std::string::npos ? value.size() + 1 : comma + 1;
    while (!item.empty() && item.back() == ' ') item.pop_back();
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    if (item.empty()) continue;
    GlusterBackend::Server s;
    if (item.front() == '[') {  // [v6]:port
      size_t close = item.find(']');
      if (close == std::string::npos) return false;
      s.host = item.substr(1, close - 1);
      std::string rest = item.substr(close + 1);
      if (!rest.empty()) {
        if (rest.front() != ':' || !parse_uint(rest.substr(1), s.port)) return false;
      }
    } else {
      size_t colon = item.find(':');
      if (colon == std::string::npos) s.host = item;
      else {
        s.host = item.substr(0, colon);
        if (!parse_uint(item.substr(colon + 1), s.port)) return false;
      }
    }
    if (s.host.empty() || s.port <= 0 || s.port > 65535) return false;
    out.push_back(std::move(s));
  }
  return !out.empty();
}

// A mistyped key or value fails startup (same policy as the local backend).
std::unique_ptr<Backend> make_gluster(const BackendConfig& cfg) {
  GlusterBackend::Config g;
  g.fsid = cfg.fsid;
  auto bad = [&](const char* key, const std::string& value) {
    LNFS_ERROR("export {}: bad gluster backend {} value '{}'", cfg.path, key, value);
    return std::unique_ptr<Backend>{};
  };
  for (const auto& [key, value] : cfg.values) {
    if (key == "volume") g.volume = value;
    else if (key == "subdir") g.subdir = value;
    else if (key == "servers" || key == "volfile_server") {
      g.servers.clear();
      if (!parse_servers(value, g.servers)) return bad(key.c_str(), value);
    } else if (key == "transport") g.transport = value;
    else if (key == "log_file") g.log_file = value;
    else if (key == "log_level") {
      if (!parse_uint(value, g.log_level) || g.log_level > 9) return bad("log_level", value);
    } else if (key == "fd_cache") {
      if (!parse_uint(value, g.fd_cache) || g.fd_cache == 0) return bad("fd_cache", value);
    } else if (key == "readdir_enrich") {
      if (!parse_bool(value, g.enrich_readdir)) return bad("readdir_enrich", value);
    } else if (key == "jukebox") {
      if (!parse_bool(value, g.jukebox)) return bad("jukebox", value);
    } else if (key == "native_locks") {
      if (!parse_bool(value, g.native_locks)) return bad("native_locks", value);
    } else {
      LNFS_ERROR("export {}: unknown gluster backend key '{}'", cfg.path, key);
      return nullptr;
    }
  }
  if (g.volume.empty()) {
    LNFS_ERROR("export {}: gluster backend needs [export.gluster] volume", cfg.path);
    return nullptr;
  }
  auto made = GlusterBackend::create(std::move(g));
  if (!made) {
    LNFS_ERROR("export {}: gluster backend config rejected: {}", cfg.path,
               errno_name(made.error()));
    return nullptr;
  }
  return std::move(*made);
}

}  // namespace

void register_gluster_backend() {
  register_backend({"gluster", kBackendApiVersion, make_gluster, /*virtual_path=*/true});
}

}  // namespace lnfs::backend
