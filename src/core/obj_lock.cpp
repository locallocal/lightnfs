#include "core/obj_lock.hpp"

namespace lnfs::core {

std::shared_ptr<rt::AsyncSharedMutex> ObjLockRegistry::get(uint32_t fsid,
                                                           const backend::ObjId& oid) {
  Key key{fsid, oid};
  size_t hash = Hash{}(key);
  Shard& shard = shards_[hash % shards_.size()];
  std::lock_guard lock(shard.mu);
  // Hit fast path (plan doc 10 §2.6): operator[] used to insert an entry on every hit
  // and a full-table sweep ran whenever the shard crossed 1024 entries.
  if (auto it = shard.locks.find(key); it != shard.locks.end()) {
    if (auto existing = it->second.lock()) return existing;
    auto made = std::make_shared<rt::AsyncSharedMutex>();
    it->second = made;  // expired entry: reuse its slot
    return made;
  }
  auto made = std::make_shared<rt::AsyncSharedMutex>();
  shard.locks.emplace(key, made);
  // Amortized cleanup: each insert past the watermark sweeps a few hash buckets (a
  // rotating cursor), so dead weak entries are pruned at O(1) per insert with no
  // full-table sweeps.
  if (shard.locks.size() > 1024) {
    Key dead[16];
    size_t ndead = 0;
    const size_t buckets = shard.locks.bucket_count();
    for (int b = 0; b < 8 && ndead < 16; ++b) {
      size_t idx = shard.sweep_pos++ % buckets;
      for (auto it = shard.locks.begin(idx); it != shard.locks.end(idx) && ndead < 16;
           ++it) {
        if (it->second.expired()) dead[ndead++] = it->first;
      }
    }
    for (size_t i = 0; i < ndead; ++i) shard.locks.erase(dead[i]);
  }
  return made;
}

}  // namespace lnfs::core
