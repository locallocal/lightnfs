#include "state/lock_mgr.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace lnfs::state {

size_t FileKeyHash::operator()(const FileKey& k) const noexcept {
  return backend::ObjIdHash{}(k.oid) ^
         (static_cast<size_t>(k.fsid) * 0x9E3779B97F4A7C15ull);
}

bool same_owner(const backend::LockOwnerId& a, const backend::LockOwnerId& b) {
  return a.len == b.len && std::memcmp(a.bytes.data(), b.bytes.data(), a.len) == 0;
}

uint64_t GatewayLockMgr::range_end(backend::LockRange r) {
  if (r.length == UINT64_MAX || r.offset > UINT64_MAX - r.length) return UINT64_MAX;
  return r.offset + r.length;
}

backend::LockRange GatewayLockMgr::to_range(uint64_t start, uint64_t end) {
  return {start, end == UINT64_MAX ? UINT64_MAX : end - start};
}

namespace {

bool overlaps(const LockSeg& s, uint64_t start, uint64_t end) {
  return s.start < end && start < s.end;
}

backend::LockConflict conflict_of(const LockSeg& s) {
  backend::LockConflict c;
  c.owner = s.owner;
  c.range = GatewayLockMgr::to_range(s.start, s.end);
  c.exclusive = s.exclusive;
  return c;
}

}  // namespace

void GatewayLockMgr::carve(std::vector<LockSeg>& segs, const backend::LockOwnerId& owner,
                           uint64_t start, uint64_t end) {
  std::vector<LockSeg> out;
  out.reserve(segs.size() + 2);
  for (const LockSeg& s : segs) {
    if (!same_owner(s.owner, owner) || !overlaps(s, start, end)) {
      out.push_back(s);
      continue;
    }
    if (s.start < start) out.push_back({s.owner, s.start, start, s.exclusive});
    if (s.end > end) out.push_back({s.owner, end, s.end, s.exclusive});
  }
  segs.swap(out);
}

void GatewayLockMgr::coalesce(std::vector<LockSeg>& segs) {
  std::sort(segs.begin(), segs.end(), [](const LockSeg& a, const LockSeg& b) {
    return a.start < b.start;
  });
  std::vector<LockSeg> out;
  for (const LockSeg& s : segs) {
    bool merged = false;
    // Adjacent/overlapping same-owner same-type segments fuse (POSIX coalescing).
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
      if (same_owner(it->owner, s.owner) && it->exclusive == s.exclusive &&
          it->end >= s.start && s.end >= it->start) {
        it->start = std::min(it->start, s.start);
        it->end = std::max(it->end, s.end);
        merged = true;
        break;
      }
    }
    if (!merged) out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const LockSeg& a, const LockSeg& b) {
    return a.start < b.start;
  });
  segs.swap(out);
}

void GatewayLockMgr::account(Shard& s, const FileKey& key, std::vector<LockSeg>& segs,
                             size_t before) {
  size_t after = segs.size();
  if (after > before) total_.fetch_add(after - before, std::memory_order_relaxed);
  else if (before > after) total_.fetch_sub(before - after, std::memory_order_relaxed);
  if (after == 0) {
    s.files.erase(key);
    files_.fetch_sub(1, std::memory_order_relaxed);
  }
}

std::optional<backend::LockConflict> GatewayLockMgr::lock(const FileKey& file,
                                                          const backend::LockOwnerId& owner,
                                                          backend::LockRange range,
                                                          bool exclusive) {
  uint64_t start = range.offset, end = range_end(range);
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto [it, inserted] = s.files.try_emplace(file);
  if (inserted) files_.fetch_add(1, std::memory_order_relaxed);
  auto& segs = it->second;
  for (const LockSeg& seg : segs) {
    if (same_owner(seg.owner, owner) || !overlaps(seg, start, end)) continue;
    if (exclusive || seg.exclusive) {
      auto c = conflict_of(seg);
      if (segs.empty()) {
        s.files.erase(it);
        files_.fetch_sub(1, std::memory_order_relaxed);
      }
      return c;
    }
  }
  size_t before = segs.size();
  carve(segs, owner, start, end);
  segs.push_back({owner, start, end, exclusive});
  coalesce(segs);
  account(s, file, segs, before);
  return std::nullopt;
}

void GatewayLockMgr::unlock(const FileKey& file, const backend::LockOwnerId& owner,
                            backend::LockRange range) {
  uint64_t start = range.offset, end = range_end(range);
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto it = s.files.find(file);
  if (it == s.files.end()) return;
  size_t before = it->second.size();
  carve(it->second, owner, start, end);
  account(s, file, it->second, before);
}

std::optional<backend::LockConflict> GatewayLockMgr::test(const FileKey& file,
                                                          const backend::LockOwnerId* owner,
                                                          backend::LockRange range,
                                                          bool exclusive) {
  uint64_t start = range.offset, end = range_end(range);
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto it = s.files.find(file);
  if (it == s.files.end()) return std::nullopt;
  for (const LockSeg& seg : it->second) {
    if ((owner && same_owner(seg.owner, *owner)) || !overlaps(seg, start, end)) continue;
    if (exclusive || seg.exclusive) return conflict_of(seg);
  }
  return std::nullopt;
}

size_t GatewayLockMgr::release_owner(const FileKey& file, const backend::LockOwnerId& owner) {
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto it = s.files.find(file);
  if (it == s.files.end()) return 0;
  size_t before = it->second.size();
  auto& segs = it->second;
  segs.erase(std::remove_if(segs.begin(), segs.end(),
                            [&](const LockSeg& seg) { return same_owner(seg.owner, owner); }),
             segs.end());
  size_t removed = before - segs.size();
  account(s, file, segs, before);
  return removed;
}

size_t GatewayLockMgr::count_owner(const FileKey& file, const backend::LockOwnerId& owner) {
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto it = s.files.find(file);
  if (it == s.files.end()) return 0;
  return static_cast<size_t>(std::count_if(
      it->second.begin(), it->second.end(),
      [&](const LockSeg& seg) { return same_owner(seg.owner, owner); }));
}

std::vector<LockSeg> GatewayLockMgr::segments(const FileKey& file) {
  Shard& s = shard(file);
  std::lock_guard g(s.mu);
  auto it = s.files.find(file);
  return it == s.files.end() ? std::vector<LockSeg>{} : it->second;
}

// ---- backend::LockMgr adapter -------------------------------------------------

rt::Task<Result<void>> GatewayLockMgr::lock(backend::Object& obj,
                                            const backend::LockOwnerId& owner,
                                            backend::LockRange range, bool exclusive,
                                            bool /*wait*/) {
  if (lock(FileKey{0, obj.id()}, owner, range, exclusive)) co_return Err(errno_from(EAGAIN));
  co_return Result<void>{};
}

rt::Task<Result<void>> GatewayLockMgr::unlock(backend::Object& obj,
                                              const backend::LockOwnerId& owner,
                                              backend::LockRange range) {
  unlock(FileKey{0, obj.id()}, owner, range);
  co_return Result<void>{};
}

rt::Task<Result<std::optional<backend::LockConflict>>> GatewayLockMgr::test(
    backend::Object& obj, backend::LockRange range, bool exclusive) {
  co_return test(FileKey{0, obj.id()}, nullptr, range, exclusive);
}

}  // namespace lnfs::state
