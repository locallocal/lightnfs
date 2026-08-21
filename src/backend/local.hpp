#pragma once

#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "backend/api.hpp"

namespace lnfs::backend {

class LocalObject;

class LocalBackend final : public Backend {
 public:
  enum class HandleMode { kAuto, kKernel, kFallback };
  enum class Identity { kCheck, kStrict, kSetFsuid };
  struct Config {
    std::string path;
    uint64_t fsid = 0;
    size_t fd_cache = 4096;
    HandleMode handles = HandleMode::kAuto;
    Identity identity = Identity::kCheck;
    bool enrich_readdir = true;
  };

  static Result<std::unique_ptr<LocalBackend>> create(Config cfg);
  ~LocalBackend() override;

  Caps caps() const override { return caps_; }
  FsLimits limits() const override { return limits_; }
  uint64_t fsid() const override { return cfg_.fsid; }
  rt::Task<Result<ObjPtr>> root() override;
  rt::Task<Result<ObjPtr>> resolve(const ObjId&) override;
  rt::Task<Result<FsStats>> statfs() override;

  bool stable_handles() const { return caps_.has(Cap::kStableHandles); }
  const std::string& root_path() const { return cfg_.path; }

  struct FdCacheStats {
    uint64_t hits = 0, misses = 0, upgrades = 0, evictions = 0;
    size_t entries = 0;
  };
  FdCacheStats fd_cache_stats() const;

  // fsync/fdatasync EIO is sticky per design 06 §6.2: after any sync failure on a file,
  // every later commit on it keeps failing instead of silently dropping writeback errors.
  void poison(const ObjId& oid);
  bool is_poisoned(const ObjId& oid) const;

 private:
  friend class LocalObject;
  class FdCache;

  LocalBackend(Config cfg, int root_fd, int mount_fd);
  Result<ObjId> oid_from_fd(int fd, std::string_view relative, bool remember = true);
  Result<int> open_oid(const ObjId& oid, int flags);
  Result<ObjPtr> object_from_fd(int fd, std::string relative, bool remember = true);
  Result<Attr> attr_from_fd(int fd) const;
  Result<DirPage> readdir_sync(const LocalObject& dir, uint64_t cookie,
                               uint32_t max_entries);
  static bool valid_name(std::string_view name, bool allow_dotdot = false);
  // Applies requested ownership to a freshly created object; EPERM from an unprivileged
  // server process is tolerated (files stay owned by the process user; documented mode).
  static void apply_created_owner(int fd, const Cred& cred);

  Config cfg_;
  int root_fd_ = -1;
  int mount_fd_ = -1;
  Caps caps_;
  FsLimits limits_;
  ObjId root_oid_{};

  std::mutex path_mu_;
  std::unordered_map<ObjId, std::string, ObjIdHash> fallback_paths_;
  struct InodeKey {
    uint64_t dev = 0;
    uint64_t ino = 0;
    friend bool operator==(const InodeKey&, const InodeKey&) = default;
  };
  struct InodeKeyHash {
    size_t operator()(const InodeKey& key) const {
      return std::hash<uint64_t>{}(key.dev) ^ (std::hash<uint64_t>{}(key.ino) << 1);
    }
  };
  std::mutex generation_mu_;
  std::unordered_map<InodeKey, uint32_t, InodeKeyHash> fallback_generations_;
  uint32_t next_fallback_generation_ = 1;
  std::unique_ptr<FdCache> fd_cache_;

  mutable std::mutex poison_mu_;
  std::unordered_set<ObjId, ObjIdHash> poisoned_;
};

class LocalObject final : public Object {
 public:
  ~LocalObject() override;

  rt::Task<Result<Attr>> getattr() override;
  rt::Task<Result<Attr>> setattr(const Cred&, const SetAttr&) override;
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
  rt::Task<Result<uint32_t>> read(OpenCtx, uint64_t off, std::span<std::byte> out,
                                  bool& eof) override;
  rt::Task<Result<uint32_t>> write(OpenCtx, uint64_t off, std::span<const std::byte> in,
                                   Stability) override;
  rt::Task<Result<void>> commit(OpenCtx, uint64_t off, uint64_t len) override;

 private:
  friend class LocalBackend;
  LocalObject(LocalBackend& backend, ObjId id, FType type, int path_fd,
              std::string relative)
      : Object(std::move(id), type), backend_(backend), path_fd_(path_fd),
        relative_(std::move(relative)) {}

  // Creation-family shared tail: wraps a just-created child as Created{obj, attr}.
  rt::Task<Result<Created>> created_child(std::string_view name);
  Result<Created> created_child_sync(std::string_view name);
  rt::Task<Result<void>> require_dir_write(const Cred& cred);

  LocalBackend& backend_;
  int path_fd_ = -1;  // O_PATH|O_NOFOLLOW, safe for statx/lookup.
  std::string relative_;
};

}  // namespace lnfs::backend
