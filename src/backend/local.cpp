#include "backend/local.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <limits>
#include <vector>

#include "runtime/io.hpp"
#include "runtime/offload_pool.hpp"

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
    explicit Entry(int value) : fd(value) {}
    ~Entry() {
      if (fd >= 0) ::close(fd);
    }
    int fd;
    uint64_t used = 0;
  };
  using Ref = std::shared_ptr<Entry>;

  FdCache(LocalBackend& backend, size_t capacity)
      : backend_(backend), per_shard_capacity_(std::max<size_t>((capacity + kShards - 1) /
                                                                    kShards,
                                                                1)) {}

  rt::Task<Result<Ref>> acquire(const ObjId& oid, int flags) {
    Shard& shard = shards_[ObjIdHash{}(oid) % kShards];
    {
      std::lock_guard lock(shard.mu);
      auto it = shard.entries.find(oid);
      if (it != shard.entries.end()) {
        it->second->used = ++shard.clock;
        co_return it->second;
      }
    }
    auto opened = co_await rt::offload([this, oid, flags] { return backend_.open_oid(oid, flags); });
    if (!opened) co_return Err(opened.error());
    auto value = std::make_shared<Entry>(*opened);
    {
      std::lock_guard lock(shard.mu);
      auto [it, inserted] = shard.entries.emplace(oid, value);
      if (!inserted) value = it->second;
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
      }
    }
    co_return value;
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
};

LocalBackend::LocalBackend(Config cfg, int root_fd, int mount_fd)
    : cfg_(std::move(cfg)), root_fd_(root_fd), mount_fd_(mount_fd) {
  caps_.set(Cap::kSymlink).set(Cap::kHardlink);
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
  return out;
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
  auto ref = co_await backend_.fd_cache_->acquire(id(), O_RDONLY);
  if (!ref) co_return Err(ref.error());
  int n = co_await rt::uring_read((*ref)->fd, out, off);
  if (n < 0) co_return Err(errno_from_neg(n));
  auto attr = co_await getattr();
  eof = attr ? off + static_cast<uint64_t>(n) >= attr->size : n == 0;
  co_return static_cast<uint32_t>(n);
}

namespace {
std::unique_ptr<Backend> make_local(const BackendConfig& cfg) {
  LocalBackend::Config local;
  local.path = cfg.path;
  local.fsid = cfg.fsid;
  if (auto it = cfg.values.find("fd_cache"); it != cfg.values.end())
    local.fd_cache = std::stoull(it->second);
  if (auto it = cfg.values.find("handles"); it != cfg.values.end()) {
    if (it->second == "kernel") local.handles = LocalBackend::HandleMode::kKernel;
    else if (it->second == "fallback") local.handles = LocalBackend::HandleMode::kFallback;
  }
  if (auto it = cfg.values.find("readdir_enrich"); it != cfg.values.end())
    local.enrich_readdir = it->second != "false";
  auto made = LocalBackend::create(std::move(local));
  return made ? std::move(*made) : nullptr;
}
}  // namespace

LNFS_REGISTER_BACKEND("local", make_local)

}  // namespace lnfs::backend
