#pragma once
// Gateway byte-range lock table (design 07 §7.6; the v1 implementer of the backend
// LockMgr interface, 05 §5.8).  Per-file sorted interval list with POSIX semantics:
// a new lock by an owner replaces that owner's segments inside its range (upgrade /
// downgrade / split) and coalesces with adjacent same-type segments; a conflicting
// lock from another owner answers with the first conflicting segment (non-blocking:
// the server never queues, RFC 8881 §18.10 — the client polls).  Pure table
// operations under a plain per-shard mutex: no IO, no coroutine suspension.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "backend/api.hpp"

namespace lnfs::state {

struct FileKey {
  uint32_t fsid = 0;
  backend::ObjId oid{};
  friend bool operator==(const FileKey&, const FileKey&) = default;
};
struct FileKeyHash {
  size_t operator()(const FileKey& k) const noexcept;
};

bool same_owner(const backend::LockOwnerId& a, const backend::LockOwnerId& b);

struct LockSeg {
  backend::LockOwnerId owner;
  uint64_t start = 0;
  uint64_t end = 0;  // exclusive; UINT64_MAX = "to EOF" (length ~0 on the wire)
  bool exclusive = false;
};

class GatewayLockMgr : public backend::LockMgr {
 public:
  // Range helpers: length UINT64_MAX means "to end of file".
  static uint64_t range_end(backend::LockRange r);
  static backend::LockRange to_range(uint64_t start, uint64_t end);

  // Grants or reports the first conflicting segment (another owner, overlapping,
  // at least one side exclusive).  Never blocks.
  std::optional<backend::LockConflict> lock(const FileKey& file,
                                            const backend::LockOwnerId& owner,
                                            backend::LockRange range, bool exclusive);
  // Removes the owner's coverage of the range (POSIX split); unlocking nothing is fine.
  void unlock(const FileKey& file, const backend::LockOwnerId& owner,
              backend::LockRange range);
  // LOCKT: conflict test on behalf of `owner` (its own locks never conflict).
  std::optional<backend::LockConflict> test(const FileKey& file,
                                            const backend::LockOwnerId* owner,
                                            backend::LockRange range, bool exclusive);
  // Drops every segment the owner holds on the file (CLOSE / FREE_STATEID / reclaim).
  size_t release_owner(const FileKey& file, const backend::LockOwnerId& owner);
  size_t count_owner(const FileKey& file, const backend::LockOwnerId& owner);
  std::vector<LockSeg> segments(const FileKey& file);
  size_t total_segments() const { return total_.load(std::memory_order_relaxed); }
  size_t files_locked() const { return files_.load(std::memory_order_relaxed); }

  // backend::LockMgr (05 §5.8) — keyed by Object::id() with fsid 0; `wait` is
  // accepted and ignored (non-blocking by design).  Conflict → Err(EAGAIN).
  rt::Task<Result<void>> lock(backend::Object&, const backend::LockOwnerId&,
                              backend::LockRange, bool exclusive, bool wait) override;
  rt::Task<Result<void>> unlock(backend::Object&, const backend::LockOwnerId&,
                                backend::LockRange) override;
  rt::Task<Result<std::optional<backend::LockConflict>>> test(backend::Object&,
                                                              backend::LockRange,
                                                              bool exclusive) override;

 private:
  static constexpr size_t kShards = 16;
  struct Shard {
    std::mutex mu;
    std::unordered_map<FileKey, std::vector<LockSeg>, FileKeyHash> files;
  };
  Shard& shard(const FileKey& key) { return shards_[FileKeyHash{}(key) % kShards]; }
  // Removes `owner`'s coverage of [start,end) from segs (splitting as needed).
  void carve(std::vector<LockSeg>& segs, const backend::LockOwnerId& owner,
             uint64_t start, uint64_t end);
  void coalesce(std::vector<LockSeg>& segs);
  void account(Shard& s, const FileKey& key, std::vector<LockSeg>& segs, size_t before);

  std::array<Shard, kShards> shards_;
  std::atomic<size_t> total_{0}, files_{0};
};

}  // namespace lnfs::state
