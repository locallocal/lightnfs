#include "backend/local.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <vector>

#include "runtime/io.hpp"
#include "runtime/offload_pool.hpp"
#include "util/log.hpp"

namespace lnfs::backend {
namespace {

constexpr std::byte kKernelHandle{1};
constexpr std::byte kFallbackHandle{2};
constexpr size_t kMaxKernelHandle = ObjId::kMax - 6;  // tag + type + byte count

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

uint32_t load_u32(std::span<const std::byte> in) {
  uint32_t out = 0;
  std::memcpy(&out, in.data(), sizeof(out));
  return out;
}

template <class T>
void append_native(std::vector<std::byte>& out, T value) {
  const auto* p = reinterpret_cast<const std::byte*>(&value);
  out.insert(out.end(), p, p + sizeof(value));
}

std::string child_path(std::string_view parent, std::string_view name) {
  if (name == ".") return std::string(parent);
  if (name == "..") {
    if (parent == ".") return ".";
    std::string out(parent);
    auto slash = out.rfind('/');
    return slash == std::string::npos ? "." : out.substr(0, slash);
  }
  return parent == "." ? std::string(name) : std::string(parent) + "/" + std::string(name);
}

struct LinuxDirent64 {
  uint64_t ino;
  int64_t off;
  uint16_t reclen;
  uint8_t type;
  char name[];
};

}  // namespace

class LocalBackend::FdCache {
 public:
  struct Entry {
    Entry(int value, int mode) : fd(value), accmode(mode) {}
    ~Entry() {
      if (fd >= 0) ::close(fd);
    }
    int fd;
    int accmode;  // O_RDONLY or O_RDWR
    uint64_t used = 0;
  };
  using Ref = std::shared_ptr<Entry>;

  FdCache(LocalBackend& backend, size_t capacity)
      : backend_(backend), per_shard_capacity_(std::max<size_t>((capacity + kShards - 1) /
                                                                    kShards,
                                                                1)) {}

  // One cached fd per object.  A read acquire reuses any entry; a write acquire needs
  // O_RDWR and upgrades an O_RDONLY entry in place (old refs drain via shared_ptr).
  // A write acquire on a read-only filesystem propagates EROFS; a read acquire never
  // asks for more than O_RDONLY, which is the documented degraded mode (06 §6.3).
  rt::Task<Result<Ref>> acquire(const ObjId& oid, bool write) {
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    {
      std::lock_guard lock(shard.mu);
      auto it = shard.entries.find(oid);
      if (it != shard.entries.end() && (!write || it->second->accmode == O_RDWR)) {
        it->second->used = ++shard.clock;
        hits_.fetch_add(1, std::memory_order_relaxed);
        co_return it->second;
      }
      (write && it != shard.entries.end() ? upgrades_ : misses_)
          .fetch_add(1, std::memory_order_relaxed);
    }
    int flags = write ? O_RDWR : O_RDONLY;
    auto opened = co_await rt::offload([this, oid, flags] { return backend_.open_oid(oid, flags); });
    if (!opened) co_return Err(opened.error());
    auto value = std::make_shared<Entry>(*opened, flags);
    {
      std::lock_guard lock(shard.mu);
      auto [it, inserted] = shard.entries.emplace(oid, value);
      if (!inserted) {
        if (write && it->second->accmode != O_RDWR) it->second = value;  // upgrade
        else value = it->second;
      }
      value->used = ++shard.clock;
      while (shard.entries.size() > per_shard_capacity_) {
        auto victim = shard.entries.end();
        for (auto i = shard.entries.begin(); i != shard.entries.end(); ++i) {
          if (i->second.use_count() != 1) continue;
          if (victim == shard.entries.end() || i->second->used < victim->second->used)
            victim = i;
        }
        if (victim == shard.entries.end()) break;
        shard.entries.erase(victim);  // Entry closes only its fd; ObjId remains valid.
        evictions_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    co_return value;
  }

  FdCacheStats stats() const {
    FdCacheStats out;
    out.hits = hits_.load(std::memory_order_relaxed);
    out.misses = misses_.load(std::memory_order_relaxed);
    out.upgrades = upgrades_.load(std::memory_order_relaxed);
    out.evictions = evictions_.load(std::memory_order_relaxed);
    for (const auto& shard : shards_) {
      std::lock_guard lock(const_cast<std::mutex&>(shard.mu));
      out.entries += shard.entries.size();
    }
    return out;
  }

 private:
  static constexpr size_t kShards = 16;
  struct Shard {
    std::mutex mu;
    std::unordered_map<ObjId, Ref, ObjIdHash> entries;
    uint64_t clock = 0;
  };
  LocalBackend& backend_;
  size_t per_shard_capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0}, misses_{0}, upgrades_{0}, evictions_{0};
};

LocalBackend::FdCacheStats LocalBackend::fd_cache_stats() const { return fd_cache_->stats(); }

void LocalBackend::poison(const ObjId& oid) {
  std::lock_guard lock(poison_mu_);
  poisoned_.insert(oid);
}

bool LocalBackend::is_poisoned(const ObjId& oid) const {
  std::lock_guard lock(poison_mu_);
  return poisoned_.contains(oid);
}

LocalBackend::LocalBackend(Config cfg, int root_fd, int mount_fd)
    : cfg_(std::move(cfg)), root_fd_(root_fd), mount_fd_(mount_fd) {
  caps_.set(Cap::kSymlink).set(Cap::kHardlink).set(Cap::kMknod);
  fd_cache_ = std::make_unique<FdCache>(*this, cfg_.fd_cache);
  long name = fpathconf(root_fd_, _PC_NAME_MAX);
  long link = fpathconf(root_fd_, _PC_LINK_MAX);
  if (name > 0) limits_.max_name = static_cast<uint32_t>(name);
  if (link > 0) limits_.max_link = static_cast<uint32_t>(link);
#ifdef STATX_CHANGE_COOKIE
  struct statx st {};
  if (::statx(root_fd_, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
              STATX_BASIC_STATS | STATX_CHANGE_COOKIE, &st) == 0 &&
      (st.stx_mask & STATX_CHANGE_COOKIE))
    caps_.set(Cap::kNativeChange);
#endif
}

Result<std::unique_ptr<LocalBackend>> LocalBackend::create(Config cfg) {
  if (cfg.path.empty() || cfg.fsid == 0) return Err(errno_from(EINVAL));
  int root = ::open(cfg.path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (root < 0) return Err(errno_from(errno));
  int mount = ::open(cfg.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (mount < 0) {
    int e = errno;
    ::close(root);
    return Err(errno_from(e));
  }
  auto out = std::unique_ptr<LocalBackend>(new LocalBackend(std::move(cfg), root, mount));

  // Probe kernel file handles once. Auto mode falls back explicitly and does not advertise
  // stable handles; forced kernel mode rejects startup instead of silently weakening P1/P2.
  auto kernel = out->oid_from_fd(root, ".", false);
  bool kernel_ok = kernel && kernel->bytes[0] == kKernelHandle;
  if (kernel_ok) {
    auto reopened = out->open_oid(*kernel, O_PATH | O_NOFOLLOW);
    kernel_ok = reopened.has_value();
    if (reopened) ::close(*reopened);
  }
  if (out->cfg_.handles == HandleMode::kKernel && !kernel_ok) {
    Errno e = kernel ? errno_from(EOPNOTSUPP) : kernel.error();
    return Err(e);
  }
  if (out->cfg_.handles == HandleMode::kFallback || !kernel_ok) {
    out->cfg_.handles = HandleMode::kFallback;
    auto fallback = out->oid_from_fd(root, ".");
    if (!fallback) return Err(fallback.error());
    out->root_oid_ = *fallback;
  } else {
    out->cfg_.handles = HandleMode::kKernel;
    out->caps_.set(Cap::kStableHandles);
    out->root_oid_ = *kernel;
  }
  out->probe_v42_caps();
  return out;
}

// Probes with two O_TMPFILE files inside the export (no namespace footprint); if the
// kernel/filesystem lacks O_TMPFILE, falls back to the documented defaults: lseek/
// fallocate are assumed (every mainstream fs), copy_file_range has a pread/pwrite
// fallback in copy_range(), and CLONE follows the fs magic (XFS/Btrfs).
void LocalBackend::probe_v42_caps() {
  caps_.set(Cap::kSparseOps).set(Cap::kCopyRange);
  int a = ::openat(mount_fd_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
  int b = a >= 0 ? ::openat(mount_fd_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600) : -1;
  if (a < 0 || b < 0) {
    if (a >= 0) ::close(a);
    struct statfs sf {};
    if (::fstatfs(mount_fd_, &sf) == 0 &&
        (sf.f_type == 0x58465342 /* XFS */ || sf.f_type == 0x9123683E /* BTRFS */))
      caps_.set(Cap::kCloneRange);
    return;
  }
  char buf[4096];
  std::memset(buf, 'x', sizeof buf);
  bool wrote = ::pwrite(a, buf, sizeof buf, 0) == static_cast<ssize_t>(sizeof buf);
  if (wrote) {
    if (::fallocate(a, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1024) < 0 &&
        errno == EOPNOTSUPP)
      caps_.clear(Cap::kSparseOps);
    if (::lseek(a, 0, SEEK_HOLE) < 0 && errno == EINVAL) caps_.clear(Cap::kSparseOps);
    struct file_clone_range range {};
    range.src_fd = a;
    range.src_offset = 0;
    range.src_length = 0;
    range.dest_offset = 0;
    if (::ioctl(b, FICLONERANGE, &range) == 0) caps_.set(Cap::kCloneRange);
  }
  ::close(a);
  ::close(b);
}

LocalBackend::~LocalBackend() {
  fd_cache_.reset();
  if (mount_fd_ >= 0) ::close(mount_fd_);
  if (root_fd_ >= 0) ::close(root_fd_);
}

Result<ObjId> LocalBackend::oid_from_fd(int fd, std::string_view relative, bool remember) {
  std::vector<std::byte> encoded;
  if (cfg_.handles != HandleMode::kFallback) {
    std::vector<std::byte> storage(sizeof(file_handle) + kMaxKernelHandle);
    auto* handle = reinterpret_cast<file_handle*>(storage.data());
    handle->handle_bytes = kMaxKernelHandle;
    int mount_id = 0;
    if (::name_to_handle_at(fd, "", handle, &mount_id, AT_EMPTY_PATH) == 0 &&
        handle->handle_bytes <= kMaxKernelHandle) {
      encoded.push_back(kKernelHandle);
      append_native(encoded, handle->handle_type);
      encoded.push_back(static_cast<std::byte>(handle->handle_bytes));
      const auto* p = reinterpret_cast<const std::byte*>(handle->f_handle);
      encoded.insert(encoded.end(), p, p + handle->handle_bytes);
      return ObjId::from(encoded);
    }
    if (cfg_.handles == HandleMode::kKernel) return Err(errno_from(errno));
  }

  struct stat st {};
  if (fstat(fd, &st) < 0) return Err(errno_from(errno));
  // Birth time is stable across content/metadata changes and changes on inode recreation,
  // making it a useful unprivileged generation hint.  Filesystems without STATX_BTIME use a
  // process-local generation keyed by dev+ino; that last-resort path is why fallback mode
  // never advertises kStableHandles.
  uint32_t generation = 0;
  struct statx sx {};
  if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0 &&
      (sx.stx_mask & STATX_BTIME)) {
    uint64_t stamp = static_cast<uint64_t>(sx.stx_btime.tv_sec) * 1000000007ull +
                     sx.stx_btime.tv_nsec;
    generation = static_cast<uint32_t>(stamp ^ (stamp >> 32));
  } else {
    std::lock_guard lock(generation_mu_);
    InodeKey key{static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)};
    auto [it, inserted] = fallback_generations_.try_emplace(key, next_fallback_generation_);
    if (inserted && ++next_fallback_generation_ == 0) ++next_fallback_generation_;
    generation = it->second;
  }
  encoded.push_back(kFallbackHandle);
  append_native(encoded, static_cast<uint64_t>(st.st_dev));
  append_native(encoded, static_cast<uint64_t>(st.st_ino));
  append_native(encoded, generation);
  auto oid = ObjId::from(encoded);
  if (oid && remember) {
    std::lock_guard lock(path_mu_);
    fallback_paths_[*oid] = relative;
  }
  return oid;
}

Result<int> LocalBackend::open_oid(const ObjId& oid, int flags) {
  auto bytes = oid.view();
  if (bytes.empty()) return Err(errno_from(ESTALE));
  flags |= O_CLOEXEC;
  if (bytes[0] == kKernelHandle) {
    if (bytes.size() < 6) return Err(errno_from(ESTALE));
    uint32_t count = static_cast<uint8_t>(bytes[5]);
    if (count == 0 || bytes.size() != 6 + count) return Err(errno_from(ESTALE));
    std::vector<std::byte> storage(sizeof(file_handle) + count);
    auto* handle = reinterpret_cast<file_handle*>(storage.data());
    handle->handle_type = static_cast<int>(load_u32(bytes.subspan(1, 4)));
    handle->handle_bytes = count;
    std::memcpy(handle->f_handle, bytes.data() + 6, count);
    int fd = ::open_by_handle_at(mount_fd_, handle, flags);
    if (fd < 0) return Err(errno_from(errno == ENOENT ? ESTALE : errno));
    return fd;
  }
  if (bytes[0] != kFallbackHandle || bytes.size() != 21) return Err(errno_from(ESTALE));
  std::string relative;
  {
    std::lock_guard lock(path_mu_);
    auto it = fallback_paths_.find(oid);
    if (it == fallback_paths_.end()) return Err(errno_from(ESTALE));
    relative = it->second;
  }
  int fd = ::openat(root_fd_, relative.c_str(), flags | O_NOFOLLOW);
  if (fd < 0) return Err(errno_from(errno == ENOENT ? ESTALE : errno));
  auto actual = oid_from_fd(fd, relative, false);
  if (!actual || *actual != oid) {
    ::close(fd);
    return Err(errno_from(ESTALE));
  }
  return fd;
}

Result<Attr> LocalBackend::attr_from_fd(int fd) const {
  struct stat st {};
  if (fstat(fd, &st) < 0) return Err(errno_from(errno));
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
  a.change = static_cast<uint64_t>(std::max<int64_t>(a.ctime.sec, 0)) * 1000000000ull +
             a.ctime.nsec;
  return a;
}

Result<ObjPtr> LocalBackend::object_from_fd(int fd, std::string relative, bool remember) {
  auto attr = attr_from_fd(fd);
  if (!attr) {
    ::close(fd);
    return Err(attr.error());
  }
  auto oid = oid_from_fd(fd, relative, remember);
  if (!oid) {
    ::close(fd);
    return Err(oid.error());
  }
  return std::static_pointer_cast<Object>(
      std::shared_ptr<LocalObject>(new LocalObject(*this, *oid, attr->type, fd,
                                                   std::move(relative))));
}

rt::Task<Result<ObjPtr>> LocalBackend::root() {
  int fd = ::dup(root_fd_);
  if (fd < 0) co_return Err(errno_from(errno));
  co_return object_from_fd(fd, ".");
}

rt::Task<Result<ObjPtr>> LocalBackend::resolve(const ObjId& oid) {
  auto opened = co_await rt::offload([this, oid] { return open_oid(oid, O_PATH | O_NOFOLLOW); });
  if (!opened) co_return Err(opened.error());
  std::string path = ".";
  if (!stable_handles()) {
    std::lock_guard lock(path_mu_);
    auto it = fallback_paths_.find(oid);
    if (it == fallback_paths_.end()) {
      ::close(*opened);
      co_return Err(errno_from(ESTALE));
    }
    path = it->second;
  }
  co_return object_from_fd(*opened, std::move(path));
}

rt::Task<Result<FsStats>> LocalBackend::statfs() {
  co_return co_await rt::offload([this]() -> Result<FsStats> {
    struct statvfs s {};
    if (fstatvfs(root_fd_, &s) < 0) return Err(errno_from(errno));
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

bool LocalBackend::valid_name(std::string_view name, bool allow_dotdot) {
  if (name.empty() || name.find('/') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos)
    return false;
  if (!allow_dotdot && (name == "." || name == "..")) return false;
  return true;
}

LocalObject::~LocalObject() {
  if (path_fd_ >= 0) ::close(path_fd_);
}

rt::Task<Result<Attr>> LocalObject::getattr() {
  struct statx st {};
  unsigned mask = STATX_BASIC_STATS;
#ifdef STATX_CHANGE_COOKIE
  mask |= STATX_CHANGE_COOKIE;
#endif
  int rc = co_await rt::uring_statx(path_fd_, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, mask,
                                    &st);
  if (rc < 0) co_return Err(errno_from_neg(rc));
  Attr a;
  a.type = mode_type(st.stx_mode);
  a.mode = st.stx_mode & 07777;
  a.nlink = st.stx_nlink;
  a.uid = st.stx_uid;
  a.gid = st.stx_gid;
  a.size = st.stx_size;
  a.used = st.stx_blocks * 512;
  a.rdev = {st.stx_rdev_major, st.stx_rdev_minor};
  a.fileid = st.stx_ino;
  a.atime = {st.stx_atime.tv_sec, st.stx_atime.tv_nsec};
  a.mtime = {st.stx_mtime.tv_sec, st.stx_mtime.tv_nsec};
  a.ctime = {st.stx_ctime.tv_sec, st.stx_ctime.tv_nsec};
  a.change = static_cast<uint64_t>(std::max<int64_t>(a.ctime.sec, 0)) * 1000000000ull +
             a.ctime.nsec;
#ifdef STATX_CHANGE_COOKIE
  if (st.stx_mask & STATX_CHANGE_COOKIE) a.change = st.stx_change_cookie;
#endif
  co_return a;
}

rt::Task<Result<ObjPtr>> LocalObject::lookup(const Cred& cred, std::string_view name) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (!LocalBackend::valid_name(name, true)) co_return Err(errno_from(EINVAL));
  auto allowed = co_await access(cred, Access::kLookup);
  if (!allowed) co_return Err(allowed.error());
  if (!allowed->has(Access::kLookup)) co_return Err(errno_from(EACCES));
  if (relative_ == "." && name == "..") co_return co_await backend_.root();

  std::string owned(name);
  int fd = co_await rt::uring_openat(path_fd_, owned.c_str(),
                                     O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
  if (fd < 0) co_return Err(errno_from_neg(fd));
  co_return backend_.object_from_fd(fd, child_path(relative_, name));
}

Result<DirPage> LocalBackend::readdir_sync(const LocalObject& dir, uint64_t cookie,
                                            uint32_t max_entries) {
  auto opened = open_oid(dir.id(), O_RDONLY | O_DIRECTORY);
  if (!opened) return Err(opened.error());
  int fd = *opened;
  if (cookie != 0 && lseek(fd, static_cast<off_t>(cookie), SEEK_SET) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  std::array<std::byte, 64 * 1024> buf{};
  DirPage page;
  while (page.ents.size() < max_entries) {
    int n = static_cast<int>(syscall(SYS_getdents64, fd, buf.data(), buf.size()));
    if (n < 0) {
      int e = errno;
      ::close(fd);
      return Err(errno_from(e));
    }
    if (n == 0) {
      page.eof = true;
      break;
    }
    size_t pos = 0;
    while (pos < static_cast<size_t>(n) && page.ents.size() < max_entries) {
      auto* ent = reinterpret_cast<const LinuxDirent64*>(buf.data() + pos);
      if (ent->reclen < offsetof(LinuxDirent64, name) + 1 || pos + ent->reclen > size_t(n)) {
        ::close(fd);
        return Err(errno_from(EIO));
      }
      size_t cap = ent->reclen - offsetof(LinuxDirent64, name);
      size_t len = strnlen(ent->name, cap);
      std::string_view name(ent->name, len);
      pos += ent->reclen;
      if (name == "." || name == "..") continue;
      DirPage::Ent out{.name = std::string(name),
                       .cookie = static_cast<uint64_t>(ent->off),
                       .fileid = ent->ino,
                       .attr = std::nullopt,
                       .oid = std::nullopt};
      if (cfg_.enrich_readdir) {
        int child = ::openat(fd, out.name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (child >= 0) {
          auto attr = attr_from_fd(child);
          if (attr) out.attr = *attr;
          auto oid = oid_from_fd(child, child_path(dir.relative_, out.name));
          if (oid) out.oid = *oid;
          ::close(child);
        }
      }
      page.ents.push_back(std::move(out));
    }
    if (page.ents.size() >= max_entries) break;
  }
  ::close(fd);
  return page;
}

rt::Task<Result<DirPage>> LocalObject::readdir(const Cred& cred, uint64_t cookie,
                                                uint32_t max_entries) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  auto allowed = co_await access(cred, Access::kRead);
  if (!allowed) co_return Err(allowed.error());
  if (!allowed->has(Access::kRead)) co_return Err(errno_from(EACCES));
  if (max_entries == 0) co_return DirPage{};
  co_return co_await rt::offload(
      [this, cookie, max_entries] { return backend_.readdir_sync(*this, cookie, max_entries); });
}

rt::Task<Result<std::string>> LocalObject::readlink() {
  if (type() != FType::kLnk) co_return Err(errno_from(EINVAL));
  co_return co_await rt::offload([this]() -> Result<std::string> {
    std::vector<char> buf(4096);
    ssize_t n = ::readlinkat(path_fd_, "", buf.data(), buf.size());
    if (n < 0) return Err(errno_from(errno));
    if (static_cast<size_t>(n) == buf.size()) return Err(errno_from(ENAMETOOLONG));
    return std::string(buf.data(), static_cast<size_t>(n));
  });
}

rt::Task<Result<uint32_t>> LocalObject::read(OpenCtx ctx, uint64_t off,
                                             std::span<std::byte> out, bool& eof) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  auto allowed = co_await access(ctx.cred, Access::kRead);
  if (!allowed) co_return Err(allowed.error());
  // Owner relaxation follows the v3 open-less convention documented in nfsv3/04.
  if (!allowed->has(Access::kRead)) {
    auto attr = co_await getattr();
    if (!attr) co_return Err(attr.error());
    if (ctx.cred.uid != attr->uid) co_return Err(errno_from(EACCES));
  }
  auto ref = co_await backend_.fd_cache_->acquire(id(), false);
  if (!ref) co_return Err(ref.error());
  int n = co_await rt::uring_read((*ref)->fd, out, off);
  if (n < 0) co_return Err(errno_from_neg(n));
  auto attr = co_await getattr();
  eof = attr ? off + static_cast<uint64_t>(n) >= attr->size : n == 0;
  co_return static_cast<uint32_t>(n);
}

namespace {

// Identity mode 2 (design 06 §6.4): switch the offload thread's filesystem ids to the
// client credential so the kernel performs authoritative permission checks.  fsuid/fsgid
// are per-thread; supplementary groups are not switched (documented limitation).
// Requires root; without privilege setfsuid is a no-op and mode 1 checks still apply.
class ScopedFsIds {
 public:
  ScopedFsIds(const Cred& cred, bool enable) {
    if (!enable || cred.uid == 0) return;
    old_gid_ = static_cast<uint32_t>(syscall(SYS_setfsgid, cred.gid));
    old_uid_ = static_cast<uint32_t>(syscall(SYS_setfsuid, cred.uid));
    active_ = true;
  }
  ~ScopedFsIds() {
    if (!active_) return;
    syscall(SYS_setfsuid, old_uid_);
    syscall(SYS_setfsgid, old_gid_);
  }
  ScopedFsIds(const ScopedFsIds&) = delete;

 private:
  bool active_ = false;
  uint32_t old_uid_ = 0, old_gid_ = 0;
};

std::string proc_fd_path(int fd) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
  return buf;
}

// EXCLUSIVE create verifier persisted across restarts in atime/mtime (design 06 §6.1,
// nfsv3/04 §8): low half in atime.tv_sec, high half in mtime.tv_sec.
void verf_split(const ExclVerf& verf, int64_t& lo, int64_t& hi) {
  uint32_t l = 0, h = 0;
  std::memcpy(&l, verf.data(), 4);
  std::memcpy(&h, verf.data() + 4, 4);
  lo = l;
  hi = h;
}

timespec to_timespec(const Timespec& t) {
  return timespec{static_cast<time_t>(t.sec), static_cast<long>(t.nsec)};
}

}  // namespace

void LocalBackend::apply_created_owner(int fd, const Cred& cred) {
  if (cred.uid == 0) return;
  // Unprivileged servers cannot chown; created objects stay owned by the process user.
  if (::fchown(fd, cred.uid, cred.gid) < 0 && errno != EPERM && errno != EINVAL) {
  }
}

rt::Task<Result<AccessMask>> LocalObject::access(const Cred& cred, AccessMask want) {
  auto base = co_await Object::access(cred, want);
  if (backend_.cfg_.identity != LocalBackend::Identity::kStrict || !base)
    co_return base;
  // Strict mode: confirm each granted bit with faccessat2(AT_EACCESS) under the client's
  // fsuid, catching ACLs and other beyond-mode-bits policy (design 06 §6.4).
  AccessMask granted = *base;
  co_return co_await rt::offload([this, cred, granted]() mutable -> Result<AccessMask> {
    ScopedFsIds ids(cred, true);
    std::string path = proc_fd_path(path_fd_);
    auto confirm = [&](Access bit, int mode) {
      if (!granted.has(bit)) return;
      if (::faccessat(AT_FDCWD, path.c_str(), mode, AT_EACCESS) != 0) granted.clear(bit);
    };
    confirm(Access::kRead, R_OK);
    confirm(Access::kModify, W_OK);
    confirm(Access::kExtend, W_OK);
    confirm(Access::kDelete, W_OK);
    confirm(Access::kLookup, X_OK);
    confirm(Access::kExecute, X_OK);
    return granted;
  });
}

rt::Task<Result<void>> LocalObject::require_dir_write(const Cred& cred) {
  if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
  if (backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid)
    co_return Result<void>{};  // the kernel enforces under the switched fsuid
  auto allowed = co_await access(cred, AccessMask{}.set(Access::kModify).set(Access::kLookup));
  if (!allowed) co_return Err(allowed.error());
  if (!allowed->has(Access::kModify) || !allowed->has(Access::kLookup))
    co_return Err(errno_from(EACCES));
  co_return Result<void>{};
}

Result<Created> LocalObject::created_child_sync(std::string_view name) {
  std::string owned(name);
  int fd = ::openat(path_fd_, owned.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return Err(errno_from(errno));
  auto attr = backend_.attr_from_fd(fd);
  if (!attr) {
    ::close(fd);
    return Err(attr.error());
  }
  auto obj = backend_.object_from_fd(fd, child_path(relative_, owned));  // consumes fd
  if (!obj) return Err(obj.error());
  return Created{std::move(*obj), *attr};
}

rt::Task<Result<Created>> LocalObject::created_child(std::string_view name) {
  std::string owned(name);
  co_return co_await rt::offload([this, owned] { return created_child_sync(owned); });
}

rt::Task<Result<Attr>> LocalObject::setattr(const Cred& cred, const SetAttr& s) {
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  if (!fsuid_mode) {
    auto attr = co_await getattr();
    if (!attr) co_return Err(attr.error());
    bool owner = cred.uid == 0 || cred.uid == attr->uid;
    if ((s.mode || s.atime_how != SetAttr::TimeHow::kOmit ||
         s.mtime_how != SetAttr::TimeHow::kOmit) &&
        !owner)
      co_return Err(errno_from(EPERM));
    if (s.uid && *s.uid != attr->uid && cred.uid != 0) co_return Err(errno_from(EPERM));
    if (s.gid && *s.gid != attr->gid && cred.uid != 0 &&
        !(owner && cred.in_group(*s.gid)))
      co_return Err(errno_from(EPERM));
    if (s.size && !owner) {
      auto allowed = co_await access(cred, Access::kModify);
      if (!allowed) co_return Err(allowed.error());
      if (!allowed->has(Access::kModify)) co_return Err(errno_from(EACCES));
    }
  }
  auto applied = co_await rt::offload([this, cred, s, fsuid_mode]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid_mode);
    int fd = -1;
    bool close_fd = false;
    if (type() == FType::kReg) {
      auto opened = backend_.open_oid(id(), s.size ? O_RDWR : O_RDONLY);
      if (!opened && s.size) return Err(opened.error());
      if (opened) {
        fd = *opened;
        close_fd = true;
      }
    } else if (type() == FType::kDir) {
      auto opened = backend_.open_oid(id(), O_RDONLY | O_DIRECTORY);
      if (opened) {
        fd = *opened;
        close_fd = true;
      }
    }
    auto finish = [&](Result<void> r) {
      if (close_fd) ::close(fd);
      return r;
    };
    if (s.size) {
      if (type() == FType::kDir) return finish(Err(errno_from(EISDIR)));
      if (type() != FType::kReg || fd < 0) return finish(Err(errno_from(EINVAL)));
      if (::ftruncate(fd, static_cast<off_t>(*s.size)) < 0)
        return finish(Err(errno_from(errno)));
    }
    if (s.mode) {
      int rc = fd >= 0 ? ::fchmod(fd, *s.mode & 07777)
                       : ::chmod(proc_fd_path(path_fd_).c_str(), *s.mode & 07777);
      if (rc < 0 && type() != FType::kLnk) return finish(Err(errno_from(errno)));
    }
    if (s.uid || s.gid) {
      uid_t uid = s.uid ? *s.uid : static_cast<uid_t>(-1);
      gid_t gid = s.gid ? *s.gid : static_cast<gid_t>(-1);
      int rc = fd >= 0 ? ::fchown(fd, uid, gid)
                       : ::fchownat(path_fd_, "", uid, gid, AT_EMPTY_PATH);
      if (rc < 0) return finish(Err(errno_from(errno)));
    }
    if (s.atime_how != SetAttr::TimeHow::kOmit || s.mtime_how != SetAttr::TimeHow::kOmit) {
      timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
      if (s.atime_how == SetAttr::TimeHow::kServer) times[0].tv_nsec = UTIME_NOW;
      else if (s.atime_how == SetAttr::TimeHow::kClient) times[0] = to_timespec(s.atime);
      if (s.mtime_how == SetAttr::TimeHow::kServer) times[1].tv_nsec = UTIME_NOW;
      else if (s.mtime_how == SetAttr::TimeHow::kClient) times[1] = to_timespec(s.mtime);
      int rc = fd >= 0 ? ::futimens(fd, times)
                       : ::utimensat(AT_FDCWD, proc_fd_path(path_fd_).c_str(), times, 0);
      if (rc < 0) return finish(Err(errno_from(errno)));
    }
    return finish({});
  });
  if (!applied) co_return Err(applied.error());
  co_return co_await getattr();
}

rt::Task<Result<Created>> LocalObject::create(const Cred& cred, std::string_view name,
                                              const SetAttr& attrs, ExclVerf* verf) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  ExclVerf verf_copy{};
  if (verf) verf_copy = *verf;
  bool exclusive = verf != nullptr;
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload(
      [this, cred, owned, attrs, verf_copy, exclusive, fsuid_mode]() -> Result<Created> {
        ScopedFsIds ids(cred, fsuid_mode);
        mode_t mode = exclusive ? 0 : (attrs.mode.value_or(0644) & 07777);
        int fd = ::openat(path_fd_, owned.c_str(),
                          O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd < 0 && errno == EEXIST && exclusive) {
          // Retransmitted EXCLUSIVE create: match the verifier stored in atime/mtime.
          struct stat st {};
          if (::fstatat(path_fd_, owned.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
              S_ISREG(st.st_mode)) {
            int64_t lo = 0, hi = 0;
            verf_split(verf_copy, lo, hi);
            if (st.st_atim.tv_sec == lo && st.st_mtim.tv_sec == hi)
              return created_child_sync(owned);
          }
          return Err(errno_from(EEXIST));
        }
        if (fd < 0) return Err(errno_from(errno));
        if (exclusive) {
          int64_t lo = 0, hi = 0;
          verf_split(verf_copy, lo, hi);
          timespec times[2] = {{static_cast<time_t>(lo), 0}, {static_cast<time_t>(hi), 0}};
          if (::futimens(fd, times) < 0) {
            int e = errno;
            ::close(fd);
            (void)::unlinkat(path_fd_, owned.c_str(), 0);
            return Err(errno_from(e));
          }
        } else {
          (void)::fchmod(fd, mode);  // exact mode regardless of process umask
          if (attrs.size && ::ftruncate(fd, static_cast<off_t>(*attrs.size)) < 0) {
            int e = errno;
            ::close(fd);
            return Err(errno_from(e));
          }
        }
        LocalBackend::apply_created_owner(fd, cred);
        auto attr = backend_.attr_from_fd(fd);
        if (!attr) {
          ::close(fd);
          return Err(attr.error());
        }
        auto obj = backend_.object_from_fd(fd, child_path(relative_, owned));
        if (!obj) return Err(obj.error());
        return Created{std::move(*obj), *attr};
      });
}

rt::Task<Result<Created>> LocalObject::mkdir(const Cred& cred, std::string_view name,
                                             const SetAttr& attrs) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  mode_t mode = attrs.mode.value_or(0755) & 07777;
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload([this, cred, owned, mode, fsuid_mode]() -> Result<Created> {
    ScopedFsIds ids(cred, fsuid_mode);
    if (::mkdirat(path_fd_, owned.c_str(), mode) < 0) return Err(errno_from(errno));
    auto created = created_child_sync(owned);
    if (!created) return created;
    int fd = ::openat(path_fd_, owned.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
      (void)::fchmod(fd, mode);
      LocalBackend::apply_created_owner(fd, cred);
      ::close(fd);
    }
    return created;
  });
}

rt::Task<Result<Created>> LocalObject::symlink(const Cred& cred, std::string_view name,
                                               std::string_view target, const SetAttr&) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  if (target.empty() || target.find('\0') != std::string_view::npos)
    co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name), owned_target(target);
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload(
      [this, cred, owned, owned_target, fsuid_mode]() -> Result<Created> {
        ScopedFsIds ids(cred, fsuid_mode);
        if (::symlinkat(owned_target.c_str(), path_fd_, owned.c_str()) < 0)
          return Err(errno_from(errno));
        if (cred.uid != 0 &&
            ::fchownat(path_fd_, owned.c_str(), cred.uid, cred.gid,
                       AT_SYMLINK_NOFOLLOW) < 0 &&
            errno != EPERM) {
        }
        return created_child_sync(owned);
      });
}

rt::Task<Result<Created>> LocalObject::mknod(const Cred& cred, std::string_view name,
                                             FType ftype, DevT dev, const SetAttr& attrs) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  mode_t type_bits = 0;
  switch (ftype) {
    case FType::kChr: type_bits = S_IFCHR; break;
    case FType::kBlk: type_bits = S_IFBLK; break;
    case FType::kSock: type_bits = S_IFSOCK; break;
    case FType::kFifo: type_bits = S_IFIFO; break;
    default: co_return Err(errno_from(EINVAL));
  }
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  mode_t mode = (attrs.mode.value_or(0644) & 07777) | type_bits;
  dev_t rdev = makedev(dev.major, dev.minor);
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload(
      [this, cred, owned, mode, rdev, fsuid_mode]() -> Result<Created> {
        ScopedFsIds ids(cred, fsuid_mode);
        if (::mknodat(path_fd_, owned.c_str(), mode, rdev) < 0)
          return Err(errno_from(errno));
        if (cred.uid != 0 &&
            ::fchownat(path_fd_, owned.c_str(), cred.uid, cred.gid,
                       AT_SYMLINK_NOFOLLOW) < 0 &&
            errno != EPERM) {
        }
        return created_child_sync(owned);
      });
}

rt::Task<Result<void>> LocalObject::unlink(const Cred& cred, std::string_view name) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload([this, cred, owned, fsuid_mode]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid_mode);
    if (::unlinkat(path_fd_, owned.c_str(), 0) < 0) {
      // POSIX says EISDIR; v3 maps directory removal via REMOVE differently than files.
      return Err(errno_from(errno));
    }
    return {};
  });
}

rt::Task<Result<void>> LocalObject::rmdir(const Cred& cred, std::string_view name) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload([this, cred, owned, fsuid_mode]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid_mode);
    if (::unlinkat(path_fd_, owned.c_str(), AT_REMOVEDIR) < 0)
      return Err(errno_from(errno));
    return {};
  });
}

rt::Task<Result<void>> LocalObject::rename(const Cred& cred, std::string_view from,
                                           Object& dst_dir, std::string_view to) {
  if (!LocalBackend::valid_name(from) || !LocalBackend::valid_name(to))
    co_return Err(errno_from(EINVAL));
  auto* dst = dynamic_cast<LocalObject*>(&dst_dir);
  if (!dst || &dst->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto src_ok = co_await require_dir_write(cred);
  if (!src_ok) co_return Err(src_ok.error());
  auto dst_ok = co_await dst->require_dir_write(cred);
  if (!dst_ok) co_return Err(dst_ok.error());
  std::string owned_from(from), owned_to(to);
  int dst_fd = dst->path_fd_;
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload(
      [this, cred, owned_from, owned_to, dst_fd, fsuid_mode]() -> Result<void> {
        ScopedFsIds ids(cred, fsuid_mode);
        if (::renameat2(path_fd_, owned_from.c_str(), dst_fd, owned_to.c_str(), 0) < 0)
          return Err(errno_from(errno));
        return {};
      });
}

rt::Task<Result<void>> LocalObject::link(const Cred& cred, Object& file,
                                         std::string_view name) {
  if (!LocalBackend::valid_name(name)) co_return Err(errno_from(EINVAL));
  auto* target = dynamic_cast<LocalObject*>(&file);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  if (target->type() == FType::kDir) co_return Err(errno_from(EPERM));
  auto dir_ok = co_await require_dir_write(cred);
  if (!dir_ok) co_return Err(dir_ok.error());
  std::string owned(name);
  int target_fd = target->path_fd_;
  bool fsuid_mode = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  co_return co_await rt::offload([this, cred, owned, target_fd, fsuid_mode]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid_mode);
    if (::linkat(target_fd, "", path_fd_, owned.c_str(), AT_EMPTY_PATH) == 0) return {};
    if (errno != EPERM && errno != EINVAL && errno != ENOENT)
      return Err(errno_from(errno));
    // AT_EMPTY_PATH linkat needs CAP_DAC_READ_SEARCH; the /proc alias works unprivileged.
    if (::linkat(AT_FDCWD, proc_fd_path(target_fd).c_str(), path_fd_, owned.c_str(),
                 AT_SYMLINK_FOLLOW) < 0)
      return Err(errno_from(errno));
    return {};
  });
}

namespace {
// Fault injection for the weekly fault-injection run (development plan §9 "故障注入":
// fsync EIO).  LNFS_FAULT_FSYNC_EIO=N makes the first N fsync/fdatasync calls of the
// process fail with EIO — exercising the sticky-poison contract end to end without a
// faulty disk.  Unset in production; read once.
std::atomic<int>& fsync_faults_left() {
  static std::atomic<int> left = [] {
    const char* v = std::getenv("LNFS_FAULT_FSYNC_EIO");
    return v ? std::atoi(v) : 0;
  }();
  return left;
}
bool take_fsync_fault() {
  auto& left = fsync_faults_left();
  int cur = left.load(std::memory_order_relaxed);
  while (cur > 0) {
    if (left.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) return true;
  }
  return false;
}
}  // namespace

rt::Task<Result<uint32_t>> LocalObject::write(OpenCtx ctx, uint64_t off,
                                              std::span<const std::byte> in,
                                              Stability stability) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  if (backend_.cfg_.identity != LocalBackend::Identity::kSetFsuid) {
    auto allowed = co_await access(ctx.cred, Access::kModify);
    if (!allowed) co_return Err(allowed.error());
    if (!allowed->has(Access::kModify)) {
      // v3 open-less owner relaxation, mirroring read (nfsv3/04 §6).
      auto attr = co_await getattr();
      if (!attr) co_return Err(attr.error());
      if (ctx.cred.uid != attr->uid) co_return Err(errno_from(EACCES));
    }
  }
  auto ref = co_await backend_.fd_cache_->acquire(id(), true);
  if (!ref) co_return Err(ref.error());
  int fd = (*ref)->fd;
  size_t done = 0;
  while (done < in.size()) {
    int n = co_await rt::uring_write(fd, in.subspan(done), off + done);
    if (n < 0) co_return Err(errno_from_neg(n));
    if (n == 0) co_return Err(errno_from(EIO));
    done += static_cast<size_t>(n);
  }
  if (stability != Stability::kUnstable) {
    int rc = take_fsync_fault() ? -EIO
                                : co_await rt::uring_fsync(fd, stability == Stability::kDataSync);
    if (rc < 0) {
      backend_.poison(id());
      co_return Err(errno_from_neg(rc));
    }
  }
  co_return static_cast<uint32_t>(done);
}

rt::Task<Result<void>> LocalObject::commit(OpenCtx, uint64_t, uint64_t) {
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  // Sticky writeback-error contract (06 §6.2): once fsync failed, keep failing.
  if (backend_.is_poisoned(id())) co_return Err(errno_from(EIO));
  auto ref = co_await backend_.fd_cache_->acquire(id(), false);
  if (!ref) co_return Err(ref.error());
  int rc = take_fsync_fault() ? -EIO : co_await rt::uring_fsync((*ref)->fd, true);
  if (rc < 0) {
    backend_.poison(id());
    co_return Err(errno_from_neg(rc));
  }
  co_return Result<void>{};
}

rt::Task<Result<void>> LocalObject::io_gate(const Cred& cred, bool write) {
  if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
  if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
  if (write && backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid)
    co_return Result<void>{};  // the kernel decides under the client's fsuid
  auto allowed = co_await access(cred, write ? Access::kModify : Access::kRead);
  if (!allowed) co_return Err(allowed.error());
  if (!allowed->has(write ? Access::kModify : Access::kRead)) {
    auto attr = co_await getattr();  // v3 open-less owner relaxation (nfsv3/04 §6)
    if (!attr) co_return Err(attr.error());
    if (cred.uid != attr->uid) co_return Err(errno_from(EACCES));
  }
  co_return Result<void>{};
}

// ---- v4.2 sweets -------------------------------------------------------------

rt::Task<Result<uint64_t>> LocalObject::seek(OpenCtx ctx, uint64_t off, SeekWhat what) {
  auto gate = co_await io_gate(ctx.cred, false);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(id(), false);
  if (!ref) co_return Err(ref.error());
  int fd = (*ref)->fd;
  co_return co_await rt::offload([fd, off, what]() -> Result<uint64_t> {
    off_t r = ::lseek(fd, static_cast<off_t>(off), what == SeekWhat::kData ? SEEK_DATA : SEEK_HOLE);
    if (r < 0) return Err(errno_from(errno));  // ENXIO past EOF / no data
    return static_cast<uint64_t>(r);
  });
}

rt::Task<Result<void>> LocalObject::allocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(id(), true);
  if (!ref) co_return Err(ref.error());
  int fd = (*ref)->fd;
  bool fsuid = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  Cred cred = ctx.cred;
  co_return co_await rt::offload([fd, off, len, fsuid, cred]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid);
    if (::fallocate(fd, 0, static_cast<off_t>(off), static_cast<off_t>(len)) < 0)
      return Err(errno_from(errno));
    return {};
  });
}

rt::Task<Result<void>> LocalObject::deallocate(OpenCtx ctx, uint64_t off, uint64_t len) {
  auto gate = co_await io_gate(ctx.cred, true);
  if (!gate) co_return Err(gate.error());
  auto ref = co_await backend_.fd_cache_->acquire(id(), true);
  if (!ref) co_return Err(ref.error());
  int fd = (*ref)->fd;
  bool fsuid = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  Cred cred = ctx.cred;
  co_return co_await rt::offload([fd, off, len, fsuid, cred]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid);
    if (::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(off),
                    static_cast<off_t>(len)) < 0)
      return Err(errno_from(errno));
    return {};
  });
}

rt::Task<Result<void>> LocalObject::clone(OpenCtx sctx, Object& dst, OpenCtx dctx,
                                          uint64_t src_off, uint64_t dst_off, uint64_t len) {
  auto* target = dynamic_cast<LocalObject*>(&dst);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto sgate = co_await io_gate(sctx.cred, false);
  if (!sgate) co_return Err(sgate.error());
  auto dgate = co_await target->io_gate(dctx.cred, true);
  if (!dgate) co_return Err(dgate.error());
  auto sref = co_await backend_.fd_cache_->acquire(id(), false);
  if (!sref) co_return Err(sref.error());
  auto dref = co_await backend_.fd_cache_->acquire(target->id(), true);
  if (!dref) co_return Err(dref.error());
  int sfd = (*sref)->fd, dfd = (*dref)->fd;
  bool fsuid = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  Cred cred = dctx.cred;
  co_return co_await rt::offload([sfd, dfd, src_off, dst_off, len, fsuid, cred]() -> Result<void> {
    ScopedFsIds ids(cred, fsuid);
    struct file_clone_range range {};
    range.src_fd = sfd;
    range.src_offset = src_off;
    range.src_length = len;  // 0 = to EOF (FICLONERANGE semantics match CLONE's)
    range.dest_offset = dst_off;
    if (::ioctl(dfd, FICLONERANGE, &range) < 0) {
      // Kernel speaks EOPNOTSUPP/EINVAL/EXDEV for "not reflinkable here"; all three
      // pass through to the engine's CLONE whitelist.
      return Err(errno_from(errno == ENOTTY ? EOPNOTSUPP : errno));
    }
    return {};
  });
}

rt::Task<Result<uint64_t>> LocalObject::copy_range(OpenCtx sctx, Object& dst, OpenCtx dctx,
                                                   uint64_t src_off, uint64_t dst_off,
                                                   uint64_t len) {
  auto* target = dynamic_cast<LocalObject*>(&dst);
  if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
  auto sgate = co_await io_gate(sctx.cred, false);
  if (!sgate) co_return Err(sgate.error());
  auto dgate = co_await target->io_gate(dctx.cred, true);
  if (!dgate) co_return Err(dgate.error());
  auto sref = co_await backend_.fd_cache_->acquire(id(), false);
  if (!sref) co_return Err(sref.error());
  auto dref = co_await backend_.fd_cache_->acquire(target->id(), true);
  if (!dref) co_return Err(dref.error());
  int sfd = (*sref)->fd, dfd = (*dref)->fd;
  bool fsuid = backend_.cfg_.identity == LocalBackend::Identity::kSetFsuid;
  Cred cred = dctx.cred;
  co_return co_await rt::offload([sfd, dfd, src_off, dst_off, len, fsuid, cred]() -> Result<uint64_t> {
    ScopedFsIds ids(cred, fsuid);
    uint64_t want = len;
    if (want == 0) {  // to EOF
      struct stat st {};
      if (::fstat(sfd, &st) < 0) return Err(errno_from(errno));
      if (static_cast<uint64_t>(st.st_size) <= src_off) return 0;
      want = static_cast<uint64_t>(st.st_size) - src_off;
    }
    uint64_t done = 0;
    bool offload_ok = true;
    while (done < want) {
      off64_t in = static_cast<off64_t>(src_off + done);
      off64_t out = static_cast<off64_t>(dst_off + done);
      size_t chunk = static_cast<size_t>(std::min<uint64_t>(want - done, 1ull << 30));
      ssize_t n = -1;
      if (offload_ok) {
        n = ::copy_file_range(sfd, &in, dfd, &out, chunk, 0);
        if (n < 0 && (errno == EXDEV || errno == EOPNOTSUPP || errno == ENOSYS ||
                      errno == EINVAL)) {
          offload_ok = false;  // fall back below (same byte semantics, more CPU)
          n = -1;
        } else if (n < 0) {
          return Err(errno_from(errno));
        }
      }
      if (!offload_ok) {
        std::vector<std::byte> buf(std::min<size_t>(chunk, 1u << 20));
        ssize_t r = ::pread(sfd, buf.data(), buf.size(), in);
        if (r < 0) return Err(errno_from(errno));
        if (r == 0) break;  // source EOF
        size_t w = 0;
        while (w < static_cast<size_t>(r)) {
          ssize_t k = ::pwrite(dfd, buf.data() + w, static_cast<size_t>(r) - w, out + w);
          if (k < 0) return Err(errno_from(errno));
          w += static_cast<size_t>(k);
        }
        n = r;
      }
      if (n == 0) break;  // source EOF
      done += static_cast<uint64_t>(n);
    }
    return done;
  });
}

namespace {
// A mistyped key or value must fail startup, not silently fall back to a default.
std::unique_ptr<Backend> make_local(const BackendConfig& cfg) {
  LocalBackend::Config local;
  local.path = cfg.path;
  local.fsid = cfg.fsid;
  for (const auto& [key, value] : cfg.values) {
    if (key == "fd_cache") {
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(),
                                       local.fd_cache);
      if (ec != std::errc{} || ptr != value.data() + value.size()) {
        LNFS_ERROR("export {}: bad local backend fd_cache value '{}'", cfg.path, value);
        return nullptr;
      }
    } else if (key == "handles") {
      if (value == "kernel") local.handles = LocalBackend::HandleMode::kKernel;
      else if (value == "fallback") local.handles = LocalBackend::HandleMode::kFallback;
      else if (value != "auto") {
        LNFS_ERROR("export {}: bad local backend handles value '{}'", cfg.path, value);
        return nullptr;
      }
    } else if (key == "readdir_enrich") {
      if (value != "true" && value != "false") {
        LNFS_ERROR("export {}: bad local backend readdir_enrich value '{}'", cfg.path,
                   value);
        return nullptr;
      }
      local.enrich_readdir = value == "true";
    } else if (key == "identity") {
      if (value == "strict") local.identity = LocalBackend::Identity::kStrict;
      else if (value == "setfsuid") local.identity = LocalBackend::Identity::kSetFsuid;
      else if (value != "check") {
        LNFS_ERROR("export {}: bad local backend identity value '{}'", cfg.path, value);
        return nullptr;
      }
    } else {
      LNFS_ERROR("export {}: unknown local backend key '{}'", cfg.path, key);
      return nullptr;
    }
  }
  auto made = LocalBackend::create(std::move(local));
  return made ? std::move(*made) : nullptr;
}
}  // namespace

void register_builtin_backends() {
  static const bool once = [] {
    register_backend({"local", kBackendApiVersion, make_local});
    return true;
  }();
  (void)once;
}

}  // namespace lnfs::backend
