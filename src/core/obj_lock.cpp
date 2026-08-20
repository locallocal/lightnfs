#include "core/obj_lock.hpp"

namespace lnfs::core {

std::shared_ptr<rt::AsyncSharedMutex> ObjLockRegistry::get(uint32_t fsid,
                                                           const backend::ObjId& oid) {
  Key key{fsid, oid};
  size_t hash = Hash{}(key);
  Shard& shard = shards_[hash % shards_.size()];
  std::lock_guard lock(shard.mu);
  if (auto existing = shard.locks[key].lock()) return existing;
  auto made = std::make_shared<rt::AsyncSharedMutex>();
  shard.locks[key] = made;
  if (shard.locks.size() > 1024) {
    for (auto it = shard.locks.begin(); it != shard.locks.end();) {
      if (it->second.expired()) it = shard.locks.erase(it);
      else ++it;
    }
  }
  return made;
}

}  // namespace lnfs::core
