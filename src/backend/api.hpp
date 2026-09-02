#pragma once
// Protocol-neutral storage boundary (design 05).  Backends speak filesystem objects,
// attributes and POSIX errno only; NFS-specific types stay in core/engines.

#include <sys/uio.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <functional>

#include "runtime/task.hpp"
#include "util/flags.hpp"
#include "util/result.hpp"
#include "util/small_vec.hpp"

namespace lnfs::backend {

inline constexpr uint32_t kBackendApiVersion = 1;

struct ObjId {
  static constexpr size_t kMax = 51;
  uint8_t len = 0;
  std::array<std::byte, kMax> bytes{};

  static Result<ObjId> from(std::span<const std::byte> value);
  std::span<const std::byte> view() const { return {bytes.data(), len}; }
  friend auto operator<=>(const ObjId&, const ObjId&) = default;
};

struct ObjIdHash {
  size_t operator()(const ObjId& id) const noexcept;
};

enum class FType : uint8_t { kReg = 1, kDir, kBlk, kChr, kLnk, kSock, kFifo };

struct DevT {
  uint32_t major = 0;
  uint32_t minor = 0;
  friend auto operator<=>(const DevT&, const DevT&) = default;
};

struct Timespec {
  int64_t sec = 0;
  uint32_t nsec = 0;
  friend auto operator<=>(const Timespec&, const Timespec&) = default;
};

struct Attr {
  FType type = FType::kReg;
  uint32_t mode = 0;
  uint32_t nlink = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint64_t size = 0;
  uint64_t used = 0;
  DevT rdev{};
  uint64_t fileid = 0;
  Timespec atime{}, mtime{}, ctime{};
  uint64_t change = 0;
};

struct SetAttr {
  std::optional<uint32_t> mode{}, uid{}, gid{};
  std::optional<uint64_t> size{};
  enum class TimeHow { kOmit, kServer, kClient };
  TimeHow atime_how = TimeHow::kOmit;
  Timespec atime{};
  TimeHow mtime_how = TimeHow::kOmit;
  Timespec mtime{};
};

struct Cred {
  uint32_t uid = 65534;
  uint32_t gid = 65534;
  std::span<const uint32_t> gids{};

  bool in_group(uint32_t group) const;
};

enum class Access : uint32_t {
  kRead = 1u << 0,
  kLookup = 1u << 1,
  kModify = 1u << 2,
  kExtend = 1u << 3,
  kDelete = 1u << 4,
  kExecute = 1u << 5,
};
using AccessMask = Flags<Access>;

struct FsStats {
  uint64_t tbytes = 0, fbytes = 0, abytes = 0;
  uint64_t tfiles = 0, ffiles = 0, afiles = 0;
};

struct FsLimits {
  uint32_t max_read = 1u << 20;
  uint32_t max_write = 1u << 20;
  uint32_t pref_read = 1u << 20;
  uint32_t pref_write = 1u << 20;
  uint32_t pref_readdir = 64u << 10;
  uint64_t max_filesize = INT64_MAX;
  uint32_t max_name = 255;
  uint32_t max_link = 32000;
  Timespec time_delta{0, 1};
};

enum class Cap : uint64_t {
  kSymlink = 1ull << 0,
  kHardlink = 1ull << 1,
  kMknod = 1ull << 2,
  kNativeAccess = 1ull << 3,
  kNativeChange = 1ull << 4,
  kStableHandles = 1ull << 5,
  kSparseOps = 1ull << 6,
  kCloneRange = 1ull << 7,
  kCopyRange = 1ull << 8,
  kByteLocks = 1ull << 9,
  kCaseInsensitive = 1ull << 10,
  kJukebox = 1ull << 11,
};
using Caps = Flags<Cap>;

enum class OpenFlag : uint32_t {
  kRead = 1u << 0,
  kWrite = 1u << 1,
  kTruncate = 1u << 2,
  kCreateUnchecked = 1u << 3,
  kCreateGuarded = 1u << 4,
  kCreateExclusive = 1u << 5,
};
using OpenFlags = Flags<OpenFlag>;
enum class Stability : uint8_t { kUnstable, kDataSync, kFileSync };
enum class SeekWhat : uint8_t { kData, kHole };
using ExclVerf = std::array<std::byte, 8>;

class OpenState {
 public:
  virtual ~OpenState() = default;
};
using OpenPtr = std::shared_ptr<OpenState>;

struct OpenCtx {
  const Cred& cred;
  OpenState* open = nullptr;
};

class Object;
using ObjPtr = std::shared_ptr<Object>;

struct Created {
  ObjPtr obj;
  Attr attr;
};

struct DirPage {
  struct Ent {
    std::string name;
    uint64_t cookie = 0;
    uint64_t fileid = 0;
    std::optional<Attr> attr;
    std::optional<ObjId> oid;
  };
  SmallVec<Ent, 64> ents;
  bool eof = false;
};

class Object {
 public:
  virtual ~Object() = default;
  const ObjId& id() const { return id_; }
  FType type() const { return type_; }

  virtual rt::Task<Result<Attr>> getattr() = 0;
  virtual rt::Task<Result<Attr>> setattr(const Cred&, const SetAttr&);
  virtual rt::Task<Result<AccessMask>> access(const Cred&, AccessMask want);

  virtual rt::Task<Result<ObjPtr>> lookup(const Cred&, std::string_view name);
  virtual rt::Task<Result<Created>> create(const Cred&, std::string_view, const SetAttr&,
                                            ExclVerf* = nullptr);
  virtual rt::Task<Result<Created>> mkdir(const Cred&, std::string_view, const SetAttr&);
  virtual rt::Task<Result<Created>> symlink(const Cred&, std::string_view, std::string_view,
                                             const SetAttr&);
  virtual rt::Task<Result<Created>> mknod(const Cred&, std::string_view, FType, DevT,
                                           const SetAttr&);
  virtual rt::Task<Result<void>> unlink(const Cred&, std::string_view);
  virtual rt::Task<Result<void>> rmdir(const Cred&, std::string_view);
  virtual rt::Task<Result<void>> rename(const Cred&, std::string_view, Object&,
                                         std::string_view);
  virtual rt::Task<Result<void>> link(const Cred&, Object&, std::string_view);
  virtual rt::Task<Result<DirPage>> readdir(const Cred&, uint64_t cookie,
                                             uint32_t max_entries);

  virtual rt::Task<Result<std::string>> readlink();
  virtual rt::Task<Result<OpenPtr>> open(const Cred&, OpenFlags);
  virtual rt::Task<Result<uint32_t>> read(OpenCtx, uint64_t, std::span<std::byte>, bool&);
  virtual rt::Task<Result<uint32_t>> write(OpenCtx, uint64_t, std::span<const std::byte>,
                                            Stability);
  // Scatter write (plan doc 10 §2.4): the engines hand the WRITE payload down as the
  // received segments so a payload spanning recv buffers is never flattened. Backends
  // with a native vectored path (local: IORING_OP_WRITEV) override; the default calls
  // the flat overload per segment with the same stability.
  virtual rt::Task<Result<uint32_t>> write(OpenCtx, uint64_t, std::span<const iovec>,
                                            Stability);
  virtual rt::Task<Result<void>> commit(OpenCtx, uint64_t, uint64_t);
  virtual rt::Task<Result<uint64_t>> seek(OpenCtx, uint64_t, SeekWhat);
  virtual rt::Task<Result<void>> allocate(OpenCtx, uint64_t, uint64_t);
  virtual rt::Task<Result<void>> deallocate(OpenCtx, uint64_t, uint64_t);
  virtual rt::Task<Result<void>> clone(OpenCtx, Object& dst, OpenCtx, uint64_t src_off,
                                        uint64_t dst_off, uint64_t len);
  virtual rt::Task<Result<uint64_t>> copy_range(OpenCtx, Object& dst, OpenCtx,
                                                 uint64_t src_off, uint64_t dst_off,
                                                 uint64_t len);

 protected:
  Object(ObjId id, FType type) : id_(std::move(id)), type_(type) {}

 private:
  ObjId id_;
  FType type_;
};

struct LockOwnerId {
  std::array<std::byte, 24> bytes{};
  uint8_t len = 0;
};
struct LockRange {
  uint64_t offset = 0, length = 0;
};
struct LockConflict {
  LockOwnerId owner;
  LockRange range;
  bool exclusive = false;
};

class LockMgr {
 public:
  virtual ~LockMgr() = default;
  virtual rt::Task<Result<void>> lock(Object&, const LockOwnerId&, LockRange, bool, bool) = 0;
  virtual rt::Task<Result<void>> unlock(Object&, const LockOwnerId&, LockRange) = 0;
  virtual rt::Task<Result<std::optional<LockConflict>>> test(Object&, LockRange, bool) = 0;
  // Drops everything `owner` holds on the object (CLOSE / stateid free / client
  // expiry).  Default: a full-range unlock; backends that pin a descriptor per owner
  // (gluster) override to close it.
  virtual rt::Task<Result<void>> release(Object&, const LockOwnerId&);
};
using LockMgrRef = std::reference_wrapper<LockMgr>;

class Backend {
 public:
  virtual ~Backend() = default;
  virtual Caps caps() const = 0;
  virtual FsLimits limits() const = 0;
  virtual uint64_t fsid() const = 0;
  virtual rt::Task<Result<ObjPtr>> root() = 0;
  virtual rt::Task<Result<ObjPtr>> resolve(const ObjId&) = 0;
  virtual rt::Task<Result<FsStats>> statfs() = 0;
  virtual rt::Task<Result<void>> start();
  virtual rt::Task<Result<void>> stop();
  virtual std::optional<LockMgrRef> native_locks() { return std::nullopt; }
};

// A dependency-free representation of one backend's TOML subtable.
struct BackendConfig {
  std::string path;
  uint64_t fsid = 0;
  std::unordered_map<std::string, std::string> values;
};

struct BackendFactory {
  std::string name;
  uint32_t api_version = kBackendApiVersion;
  std::unique_ptr<Backend> (*make)(const BackendConfig&) = nullptr;
  // The export `path` is only the name clients mount, not a directory on this host
  // (cluster backends: the tree lives in the volume) — config validation skips the
  // local stat() for these.
  bool virtual_path = false;
};

void register_backend(BackendFactory factory);
const BackendFactory* find_backend(std::string_view name);
std::vector<std::string> registered_backends();

// Registers the backends compiled into this binary (idempotent). Config loading calls
// this before find_backend: with lnfs_core linked as a static archive, nothing else
// guarantees the built-in backends' registrar TUs are pulled into the link.
void register_builtin_backends();

#define LNFS_BACKEND_CAT_(a, b) a##b
#define LNFS_BACKEND_CAT(a, b) LNFS_BACKEND_CAT_(a, b)
#define LNFS_REGISTER_BACKEND(name_literal, make_fn)                                  \
  namespace {                                                                         \
  const bool LNFS_BACKEND_CAT(lnfs_backend_registered_, __COUNTER__) = [] {           \
    ::lnfs::backend::register_backend(                                                 \
        {name_literal, ::lnfs::backend::kBackendApiVersion, make_fn});                 \
    return true;                                                                       \
  }();                                                                                 \
  }

}  // namespace lnfs::backend
