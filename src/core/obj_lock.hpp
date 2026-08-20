#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "backend/api.hpp"
#include "runtime/sync.hpp"

namespace lnfs::core {

class ObjLockRegistry {
 public:
  std::shared_ptr<rt::AsyncSharedMutex> get(uint32_t fsid, const backend::ObjId& oid);

 private:
  struct Key {
    uint32_t fsid;
    backend::ObjId oid;
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct Hash {
    size_t operator()(const Key& key) const {
      return backend::ObjIdHash{}(key.oid) ^ (static_cast<size_t>(key.fsid) << 1);
    }
  };
  struct Shard {
    std::mutex mu;
    std::unordered_map<Key, std::weak_ptr<rt::AsyncSharedMutex>, Hash> locks;
  };
  std::array<Shard, 64> shards_;
};

}  // namespace lnfs::core
