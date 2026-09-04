#include "backend/lustre/lustre.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>

#include "runtime/offload_pool.hpp"
#include "util/log.hpp"

namespace lnfs::backend {
namespace {

constexpr std::byte kLustreHandle{4};  // local uses 1/2, gluster 3
constexpr size_t kHandleLen = 1 + sizeof(llapi::Fid);

// The mount root is the highest ancestor on the same device.  Bind mounts of a
// subtree share the device with the real mount, which is why `mount` can be given
// explicitly in the config.
std::string find_mount_root(const std::string& path) {
  std::error_code ec;
  std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
  if (ec) p = path;
  struct stat st {};
  if (::stat(p.c_str(), &st) < 0) return p.string();
  while (p.has_parent_path() && p.parent_path() != p) {
    struct stat ps {};
    if (::stat(p.parent_path().c_str(), &ps) < 0 || ps.st_dev != st.st_dev) break;
    p = p.parent_path();
  }
  return p.string();
}

void note_hsm_error(const char* op, Errno e) {
  static std::atomic<uint64_t> count{0};
  uint64_t n = ++count;
  if ((n & (n - 1)) == 0)
    LNFS_WARN("lustre hsm {} failed: {} ({} occurrences total)", op, errno_name(e), n);
}

}  // namespace

// ---- handle codec ------------------------------------------------------------------

ObjId LustreBackend::oid_from_fid(const llapi::Fid& fid) {
  std::array<std::byte, kHandleLen> bytes{};
  bytes[0] = kLustreHandle;
  std::memcpy(bytes.data() + 1, &fid.seq, sizeof fid.seq);
  std::memcpy(bytes.data() + 9, &fid.oid, sizeof fid.oid);
  std::memcpy(bytes.data() + 13, &fid.ver, sizeof fid.ver);
  return *ObjId::from(bytes);
}

Result<llapi::Fid> LustreBackend::fid_from_oid(const ObjId& oid) {
  auto bytes = oid.view();
  if (bytes.size() != kHandleLen || bytes[0] != kLustreHandle) return Err(errno_from(ESTALE));
  llapi::Fid fid;
  std::memcpy(&fid.seq, bytes.data() + 1, sizeof fid.seq);
  std::memcpy(&fid.oid, bytes.data() + 9, sizeof fid.oid);
  std::memcpy(&fid.ver, bytes.data() + 13, sizeof fid.ver);
  if (fid.seq == 0 && fid.oid == 0) return Err(errno_from(ESTALE));  // FID_ZERO
  return fid;
}

// ---- backend -----------------------------------------------------------------------

LustreBackend::LustreBackend(LocalBackend::Config base, int root_fd, int mount_fd,
                             Config lcfg, const llapi::Ops& ops, int lustre_fd,
                             std::string mount_path)
    : LocalBackend(std::move(base), root_fd, mount_fd), lcfg_(std::move(lcfg)), ops_(ops),
      lustre_fd_(lustre_fd), mount_path_(std::move(mount_path)) {}

LustreBackend::~LustreBackend() {
  locks_.reset();
  // The base destructor drains the fd caches (their entries may still be opened by
  // FID through lustre_fd_'s namespace; the descriptors themselves are independent).
  if (lustre_fd_ >= 0) ::close(lustre_fd_);
}

Result<std::unique_ptr<LustreBackend>> LustreBackend::create(Config cfg,
                                                             const llapi::Ops* ops) {
  const llapi::Ops& o = ops ? *ops : llapi::kernel_ops();
  LocalBackend::Config base;
  base.path = cfg.path;
  base.fsid = cfg.fsid;
  base.fd_cache = cfg.fd_cache;
  base.handles = HandleMode::kKernel;  // handles are FIDs; the fallback tables stay unused
  base.identity = cfg.identity;
  base.enrich_readdir = cfg.enrich_readdir;
  auto [root, mount] = LNFS_TRY(open_roots(base));
  auto fail = [&](int e) {
    ::close(root);
    ::close(mount);
    return Err(errno_from(e));
  };

  std::string mount_path = cfg.mount.empty() ? find_mount_root(cfg.path) : cfg.mount;
  int lfd = ::open(mount_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (lfd < 0) {
    LNFS_ERROR("lustre export {}: cannot open mount root {}: errno={}", cfg.path,
               mount_path, errno);
    return fail(errno);
  }
  struct stat root_st {}, mount_st {};
  if (::fstat(root, &root_st) < 0 || ::fstat(lfd, &mount_st) < 0) {
    int e = errno;
    ::close(lfd);
    return fail(e);
  }
  if (root_st.st_dev != mount_st.st_dev) {
    LNFS_ERROR("lustre export {}: not inside mount root {} (different device)", cfg.path,
               mount_path);
    ::close(lfd);
    return fail(EXDEV);
  }
  if (!o.is_lustre(lfd)) {
    LNFS_ERROR("lustre export {}: {} is not a Lustre mount (statfs magic)", cfg.path,
               mount_path);
    ::close(lfd);
    return fail(EOPNOTSUPP);
  }

  auto out = std::unique_ptr<LustreBackend>(
      new LustreBackend(std::move(base), root, mount, cfg, o, lfd, std::move(mount_path)));
  auto root_fid = o.fid_of(root);
  if (!root_fid) {
    LNFS_ERROR("lustre export {}: cannot derive the root FID: {}", cfg.path,
               errno_name(root_fid.error()));
    return Err(errno_from(EOPNOTSUPP));
  }
  out->root_oid_ = oid_from_fid(*root_fid);
  // Round trip once at startup: an export whose FIDs cannot be opened back would
  // serve every filehandle as ESTALE.
  auto reopened = out->open_oid(out->root_oid_, O_PATH | O_NOFOLLOW);
  if (!reopened) {
    LNFS_ERROR("lustre export {}: .lustre/fid open of the root FID {} failed: {}", cfg.path,
               llapi::fid_to_string(*root_fid), errno_name(reopened.error()));
    return Err(errno_from(EOPNOTSUPP));
  }
  ::close(*reopened);

  out->caps_.set(Cap::kStableHandles);
  if (cfg.hsm) out->caps_.set(Cap::kJukebox);
  if (cfg.native_locks) {
    out->caps_.set(Cap::kByteLocks);
    out->locks_ = std::make_unique<LustreLockMgr>(*out);
  }
  out->probe_v42_caps();
  // Stripe-aware transfer hints (06 §6.5): the export root's default layout is what
  // new files get; a whole-stripe READ/WRITE lands on one OST.
  if (auto stripe = o.stripe_size(mount)) {
    uint32_t pref = std::clamp<uint32_t>(*stripe, 4096, out->limits_.max_read);
    out->limits_.pref_read = pref;
    out->limits_.pref_write = std::clamp<uint32_t>(*stripe, 4096, out->limits_.max_write);
  }
  LNFS_INFO("lustre export {}: mount root {} root fid {} stripe-pref {} hsm={} locks={}",
            cfg.path, out->mount_path_, llapi::fid_to_string(*root_fid),
            out->limits_.pref_write, cfg.hsm, cfg.native_locks);
  return out;
}

rt::Task<Result<void>> LustreBackend::stop() {
  if (locks_) locks_->close_all();
  co_return Result<void>{};
}

std::optional<LockMgrRef> LustreBackend::native_locks() {
  if (!locks_) return std::nullopt;
  return LockMgrRef(*locks_);
}

LustreBackend::Stats LustreBackend::stats() const {
  Stats s;
  s.jukebox = jukebox_.load(std::memory_order_relaxed);
  s.hsm_checks = hsm_checks_.load(std::memory_order_relaxed);
  s.hsm_restores = hsm_restores_.load(std::memory_order_relaxed);
  s.lock_fds = locks_ ? locks_->fds() : 0;
  return s;
}

Result<ObjId> LustreBackend::oid_from_fd(int fd, std::string_view, bool) {
  auto fid = ops_.fid_of(fd);
  if (!fid) return Err(fid.error());
  return oid_from_fid(*fid);
}

Result<int> LustreBackend::open_oid(const ObjId& oid, int flags) {
  auto fid = fid_from_oid(oid);
  if (!fid) return Err(fid.error());
  flags |= O_CLOEXEC;
  auto fd = ops_.open_by_fid(lustre_fd_, *fid, flags);
  if (!fd) {
    Errno e = fd.error();
    // Gone (ENOENT), or a FID string the MDT would not parse: both are "stale handle".
    if (e == errno_from(ENOENT) || e == errno_from(EINVAL) || e == errno_from(ENAMETOOLONG))
      return Err(errno_from(ESTALE));
    return Err(e);
  }
  if (lcfg_.hsm && !(flags & O_PATH)) {
    auto gate = hsm_gate(*fd, *fid);
    if (!gate) {
      ::close(*fd);
      return Err(gate.error());
    }
  }
  return *fd;
}

Result<void> LustreBackend::hsm_gate(int fd, const llapi::Fid& fid) {
  struct stat st {};
  if (::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) return {};
  hsm_checks_.fetch_add(1, std::memory_order_relaxed);
  auto state = ops_.hsm_state(fd);
  if (!state) return {};  // no HSM on this client: nothing can be released
  if (!(state->states & llapi::kHsReleased)) return {};
  // A read/write/truncate on a released file makes the kernel restore it inline and
  // blocks the caller for the whole restore — that would park an offload worker (or
  // an io_uring worker) for minutes.  Kick the restore explicitly instead and tell
  // the client to come back (v3 JUKEBOX / v4 DELAY); once restored, the open goes
  // through and the fd gets cached like any other.
  if (state->in_progress_state == llapi::kHpsNone) {
    hsm_restores_.fetch_add(1, std::memory_order_relaxed);
    auto kicked = ops_.hsm_restore(lustre_fd_, fid);
    // EALREADY-class answers are fine (someone else asked first); anything else is
    // worth a throttled warning because the client will keep retrying.
    if (!kicked && kicked.error() != errno_from(EALREADY) &&
        kicked.error() != errno_from(EINPROGRESS))
      note_hsm_error("restore request", kicked.error());
  }
  jukebox_.fetch_add(1, std::memory_order_relaxed);
  return Err(Errno::kJukebox);
}

// ---- native byte-range locks -------------------------------------------------------

size_t LustreLockMgr::KeyHash::operator()(const Key& k) const noexcept {
  return ObjIdHash{}(k.oid) ^ (std::hash<std::string>{}(k.owner) << 1);
}

LustreLockMgr::~LustreLockMgr() { close_all(); }

void LustreLockMgr::close_all() {
  std::lock_guard lock(mu_);
  for (auto& [key, fd] : fds_) ::close(fd);
  fds_.clear();
}

size_t LustreLockMgr::fds() const {
  std::lock_guard lock(mu_);
  return fds_.size();
}

struct flock LustreLockMgr::make_flock(LockRange range, short type) {
  struct flock fl {};
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = static_cast<off_t>(range.offset);
  // Wire "to EOF" (length ~0 / UINT64_MAX) is l_len 0; anything overflowing off_t is
  // clamped to EOF as well.
  uint64_t end = range.length == UINT64_MAX ? UINT64_MAX : range.offset + range.length;
  if (range.length == 0 || range.length == UINT64_MAX || end < range.offset ||
      end > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    fl.l_len = 0;
  else
    fl.l_len = static_cast<off_t>(range.length);
  fl.l_pid = 0;  // required for the OFD commands
  return fl;
}

// Runs on an offload worker.  Lock descriptors are opened as the gateway identity:
// the state layer already checked that the OPEN's access mode covers the lock type
// (RFC 8881 §18.10.3), and fcntl locks do not re-check permissions.
Result<int> LustreLockMgr::fd_for(const ObjId& oid, const LockOwnerId& owner, bool create) {
  Key key{oid, std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
  {
    std::lock_guard lock(mu_);
    auto it = fds_.find(key);
    if (it != fds_.end()) return it->second;
  }
  if (!create) return Err(errno_from(ENOENT));
  auto fd = backend_.open_oid(oid, O_RDWR);
  if (!fd && (fd.error() == errno_from(EACCES) || fd.error() == errno_from(EROFS) ||
              fd.error() == errno_from(EPERM)))
    fd = backend_.open_oid(oid, O_RDONLY);
  if (!fd) return Err(fd.error());
  std::lock_guard lock(mu_);
  auto [it, inserted] = fds_.emplace(key, *fd);
  if (!inserted) {  // lost a race: keep the winner
    ::close(*fd);
    return it->second;
  }
  return *fd;
}

rt::Task<Result<void>> LustreLockMgr::lock(Object& object, const LockOwnerId& owner,
                                           LockRange range, bool exclusive, bool wait) {
  auto* obj = dynamic_cast<LocalObject*>(&object);
  if (!obj || &obj->backend() != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  ObjId oid = obj->id();
  co_return co_await rt::offload([this, oid, owner, range, exclusive, wait]() -> Result<void> {
    auto fd = fd_for(oid, owner, true);
    if (!fd) return Err(fd.error());
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    // The state layer never blocks a request on a lock (RFC 8881 §18.10: clients
    // poll / get CB_NOTIFY_LOCK), so `wait` is accepted but not honoured.
    (void)wait;
    if (::fcntl(*fd, F_OFD_SETLK, &fl) < 0) {
      int e = errno;
      if (e == EAGAIN || e == EACCES) return Err(errno_from(EAGAIN));  // conflict
      return Err(errno_from(e));
    }
    return {};
  });
}

rt::Task<Result<void>> LustreLockMgr::unlock(Object& object, const LockOwnerId& owner,
                                             LockRange range) {
  auto* obj = dynamic_cast<LocalObject*>(&object);
  if (!obj || &obj->backend() != &backend_) co_return Err(errno_from(EXDEV));
  ObjId oid = obj->id();
  co_return co_await rt::offload([this, oid, owner, range]() -> Result<void> {
    auto fd = fd_for(oid, owner, false);
    if (!fd) return {};  // nothing held by this owner on this file: unlocking is idempotent
    struct flock fl = make_flock(range, F_UNLCK);
    if (::fcntl(*fd, F_OFD_SETLK, &fl) < 0) return Err(errno_from(errno));
    return {};
  });
}

rt::Task<Result<std::optional<LockConflict>>> LustreLockMgr::test(Object& object,
                                                                  LockRange range,
                                                                  bool exclusive) {
  auto* obj = dynamic_cast<LocalObject*>(&object);
  if (!obj || &obj->backend() != &backend_) co_return Err(errno_from(EXDEV));
  if (obj->type() != FType::kReg) co_return Err(errno_from(EINVAL));
  ObjId oid = obj->id();
  using Probe = Result<std::optional<LockConflict>>;
  co_return co_await rt::offload([this, oid, range, exclusive]() -> Probe {
    // A fresh descriptor is its own OFD owner, so F_OFD_GETLK reports any holder:
    // another owner on this gateway, another gateway, or a native Lustre client.
    auto fd = backend_.open_oid(oid, O_RDONLY);
    if (!fd) return Err(fd.error());
    struct flock fl = make_flock(range, exclusive ? F_WRLCK : F_RDLCK);
    int rc = ::fcntl(*fd, F_OFD_GETLK, &fl);
    int e = errno;
    ::close(*fd);
    if (rc < 0) return Err(errno_from(e));
    if (fl.l_type == F_UNLCK) return std::optional<LockConflict>{};
    LockConflict c;
    c.exclusive = fl.l_type == F_WRLCK;
    c.range.offset = static_cast<uint64_t>(fl.l_start);
    c.range.length = fl.l_len == 0 ? UINT64_MAX : static_cast<uint64_t>(fl.l_len);
    // The holder's identity is not reported for OFD locks (l_pid is -1); an empty
    // owner tells the state layer "someone else, possibly on another gateway".
    return std::optional<LockConflict>(c);
  });
}

rt::Task<Result<void>> LustreLockMgr::release(Object& object, const LockOwnerId& owner) {
  auto* obj = dynamic_cast<LocalObject*>(&object);
  if (!obj || &obj->backend() != &backend_) co_return Err(errno_from(EXDEV));
  ObjId oid = obj->id();
  co_return co_await rt::offload([this, oid, owner]() -> Result<void> {
    Key key{oid, std::string(reinterpret_cast<const char*>(owner.bytes.data()), owner.len)};
    int fd = -1;
    {
      std::lock_guard lock(mu_);
      auto it = fds_.find(key);
      if (it == fds_.end()) return {};
      fd = it->second;
      fds_.erase(it);
    }
    // Closing the last descriptor of an OFD drops its locks; the explicit unlock
    // first keeps the MDS view exact even if the close is delayed.
    struct flock fl = make_flock({0, UINT64_MAX}, F_UNLCK);
    (void)::fcntl(fd, F_OFD_SETLK, &fl);
    ::close(fd);
    return {};
  });
}

// ---- factory -----------------------------------------------------------------------

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

// A mistyped key or value fails startup (same policy as the local backend).
std::unique_ptr<Backend> make_lustre(const BackendConfig& cfg) {
  LustreBackend::Config l;
  l.path = cfg.path;
  l.fsid = cfg.fsid;
  auto bad = [&](const char* key, const std::string& value) {
    LNFS_ERROR("export {}: bad lustre backend {} value '{}'", cfg.path, key, value);
    return std::unique_ptr<Backend>{};
  };
  for (const auto& [key, value] : cfg.values) {
    if (key == "mount") l.mount = value;
    else if (key == "fd_cache") {
      if (!parse_uint(value, l.fd_cache) || l.fd_cache == 0) return bad("fd_cache", value);
    } else if (key == "identity") {
      if (value == "check") l.identity = LocalBackend::Identity::kCheck;
      else if (value == "strict") l.identity = LocalBackend::Identity::kStrict;
      else if (value == "setfsuid") l.identity = LocalBackend::Identity::kSetFsuid;
      else return bad("identity", value);
    } else if (key == "readdir_enrich") {
      if (!parse_bool(value, l.enrich_readdir)) return bad("readdir_enrich", value);
    } else if (key == "hsm") {
      if (!parse_bool(value, l.hsm)) return bad("hsm", value);
    } else if (key == "native_locks") {
      if (!parse_bool(value, l.native_locks)) return bad("native_locks", value);
    } else {
      LNFS_ERROR("export {}: unknown lustre backend key '{}'", cfg.path, key);
      return nullptr;
    }
  }
  auto made = LustreBackend::create(std::move(l));
  if (!made) {
    LNFS_ERROR("export {}: lustre backend rejected: {}", cfg.path, errno_name(made.error()));
    return nullptr;
  }
  return std::move(*made);
}

}  // namespace

void register_lustre_backend() {
  register_backend({"lustre", kBackendApiVersion, make_lustre});
}

}  // namespace lnfs::backend
