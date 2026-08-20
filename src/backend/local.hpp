#pragma once

#include <list>
#include <mutex>
#include <unordered_map>

#include "backend/api.hpp"

namespace lnfs::backend {

class LocalObject;

class LocalBackend final : public Backend {
 public:
  enum class HandleMode { kAuto, kKernel, kFallback };
  struct Config {
    std::string path;
    uint64_t fsid = 0;
    size_t fd_cache = 4096;
    HandleMode handles = HandleMode::kAuto;
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
};

class LocalObject final : public Object {
 public:
  ~LocalObject() override;

  rt::Task<Result<Attr>> getattr() override;
  rt::Task<Result<ObjPtr>> lookup(const Cred&, std::string_view name) override;
  rt::Task<Result<DirPage>> readdir(const Cred&, uint64_t cookie,
                                     uint32_t max_entries) override;
  rt::Task<Result<std::string>> readlink() override;
  rt::Task<Result<uint32_t>> read(OpenCtx, uint64_t off, std::span<std::byte> out,
                                  bool& eof) override;

 private:
  friend class LocalBackend;
  LocalObject(LocalBackend& backend, ObjId id, FType type, int path_fd,
              std::string relative)
      : Object(std::move(id), type), backend_(backend), path_fd_(path_fd),
        relative_(std::move(relative)) {}

  LocalBackend& backend_;
  int path_fd_ = -1;  // O_PATH|O_NOFOLLOW, safe for statx/lookup.
  std::string relative_;
};

}  // namespace lnfs::backend
