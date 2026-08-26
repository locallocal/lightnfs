#pragma once

#include <atomic>
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
    // Hard caps on the fallback-mode bookkeeping tables (plan doc 10 §1.5): past the
    // cap an arbitrary entry is dropped, so an evicted handle resolves to ESTALE and
    // the client re-lookups.  Fallback handles are already restart-unstable, so this
    // only trades unbounded memory for bounded staleness.
    size_t max_fallback_entries = 262144;
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
  const Config& config() const { return cfg_; }

  struct FdCacheStats {
    uint64_t hits = 0, misses = 0, upgrades = 0, evictions = 0;
    uint64_t overflows = 0;  // eviction passes that found every entry in use
    size_t entries = 0;
    // Parallel O_PATH resolve cache (plan doc 10 §2.1).
    uint64_t path_hits = 0, path_misses = 0;
    size_t path_entries = 0;
  };
  FdCacheStats fd_cache_stats() const;

  // fsync/fdatasync EIO is sticky per design 06 §6.2: after any sync failure on a file,
  // every later commit on it keeps failing instead of silently dropping writeback errors.
  void poison(const ObjId& oid);
  bool is_poisoned(const ObjId& oid) const;
  // Operator override (lightnfs-ctl clear-poison): drops every sticky mark so COMMIT
  // can succeed again without a restart; returns how many were cleared.
  size_t clear_poison();

  size_t fallback_path_count() const;  // observability for the §1.5 cap

 private:
  friend class LocalObject;
  class FdCache;
  class PathCache;

  LocalBackend(Config cfg, int root_fd, int mount_fd);
  Result<ObjId> oid_from_fd(int fd, std::string_view relative, bool remember = true);
  Result<int> open_oid(const ObjId& oid, int flags);
  Result<ObjPtr> object_from_fd(int fd, std::string relative, bool remember = true);
  Result<Attr> attr_from_fd(int fd) const;
  Result<DirPage> readdir_sync(const LocalObject& dir, uint64_t cookie,
                               uint32_t max_entries);
  static bool valid_name(std::string_view name, bool allow_dotdot = false);
  // Applies requested ownership to a freshly created object; EPERM from an unprivileged
  // server process is tolerated (files stay owned by the process user; documented mode)
  // but counted and warned about (plan doc 10 §1.8) instead of vanishing silently.
  static void apply_created_owner(int fd, const Cred& cred);
  // Throttled visibility for creation-path chown/chmod failures that must not fail the
  // operation itself: warns on the 1st, 2nd, 4th, ... occurrence process-wide.
  static void note_attr_error(const char* op, int err);
  // Runtime probe of the v4.2 capability bits (kSparseOps / kCopyRange / kCloneRange)
  // on the export filesystem; result is logged by the caller at startup.
  void probe_v42_caps();

  Config cfg_;
  int root_fd_ = -1;
  int mount_fd_ = -1;
  Caps caps_;
  FsLimits limits_;
  ObjId root_oid_{};

  std::mutex path_mu_;
  std::unordered_map<ObjId, std::string, ObjIdHash> fallback_paths_;
  std::atomic<uint64_t> fallback_evictions_{0};
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
  std::unique_ptr<PathCache> path_cache_;

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
  // v4.2 sweets (design 06 §6.x mapping table): lseek(SEEK_DATA/HOLE), fallocate(0 /
  // PUNCH_HOLE|KEEP_SIZE), ioctl(FICLONERANGE), copy_file_range with a pread/pwrite
  // fallback — all on the offload pool, fds from the fd cache.
  rt::Task<Result<uint64_t>> seek(OpenCtx, uint64_t off, SeekWhat) override;
  rt::Task<Result<void>> allocate(OpenCtx, uint64_t off, uint64_t len) override;
  rt::Task<Result<void>> deallocate(OpenCtx, uint64_t off, uint64_t len) override;
  rt::Task<Result<void>> clone(OpenCtx, Object& dst, OpenCtx, uint64_t src_off,
                               uint64_t dst_off, uint64_t len) override;
  rt::Task<Result<uint64_t>> copy_range(OpenCtx, Object& dst, OpenCtx, uint64_t src_off,
                                        uint64_t dst_off, uint64_t len) override;

 private:
  friend class LocalBackend;
  // `keeper` non-null: path_fd is borrowed from the resolve cache (plan doc 10 §2.1)
  // and stays open as long as the keeper lives; null: this object owns and closes it.
  LocalObject(LocalBackend& backend, ObjId id, FType type, int path_fd,
              std::string relative, std::shared_ptr<void> keeper = nullptr)
      : Object(std::move(id), type), backend_(backend), path_fd_(path_fd),
        relative_(std::move(relative)), keeper_(std::move(keeper)) {}

  // Creation-family shared tail: wraps a just-created child as Created{obj, attr}.
  rt::Task<Result<Created>> created_child(std::string_view name);
  Result<Created> created_child_sync(std::string_view name);
  rt::Task<Result<void>> require_dir_write(const Cred& cred);
  // Regular-file precondition + the read/write permission gate shared by read/write and
  // the v4.2 ops (owner relaxation; skipped for writes in setfsuid identity mode).
  rt::Task<Result<void>> io_gate(const Cred& cred, bool write);

  LocalBackend& backend_;
  int path_fd_ = -1;  // O_PATH|O_NOFOLLOW, safe for statx/lookup.
  std::string relative_;
  std::shared_ptr<void> keeper_;  // set: path_fd_ belongs to the resolve cache
};

}  // namespace lnfs::backend
