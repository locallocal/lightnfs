#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include "backend/api.hpp"

namespace lnfs::backend {

// Read-only in-memory backend used by engine tests and full-path benchmarks.  Directory
// cookies are allocation sequence numbers, so insertion/deletion never renumbers survivors.
class MemoryBackend final : public Backend {
 public:
  explicit MemoryBackend(uint64_t fsid = 1);

  Caps caps() const override;
  FsLimits limits() const override { return {}; }
  uint64_t fsid() const override { return fsid_; }
  rt::Task<Result<ObjPtr>> root() override;
  rt::Task<Result<ObjPtr>> resolve(const ObjId&) override;
  rt::Task<Result<FsStats>> statfs() override;

  Result<ObjId> add_dir(std::string_view path, uint32_t mode = 0755);
  Result<ObjId> add_file(std::string_view path, std::span<const std::byte> data,
                         uint32_t mode = 0644);
  Result<ObjId> add_file(std::string_view path, std::string_view data,
                         uint32_t mode = 0644) {
    return add_file(path, std::span<const std::byte>(
                              reinterpret_cast<const std::byte*>(data.data()), data.size()),
                    mode);
  }
  Result<ObjId> add_symlink(std::string_view path, std::string_view target);

 private:
  struct Node;
  class MemoryObject;
  friend class MemoryObject;

  ObjId id_for(uint64_t id) const;
  std::shared_ptr<Node> find_path(std::string_view path);
  Result<ObjId> add_node(std::string_view path, FType type, uint32_t mode,
                          std::span<const std::byte> data, std::string_view link);
  ObjPtr wrap(const std::shared_ptr<Node>& node);

  uint64_t fsid_;
  mutable std::mutex mu_;
  uint64_t next_id_ = 2;
  uint64_t next_cookie_ = 1;
  std::shared_ptr<Node> root_;
  std::unordered_map<ObjId, std::weak_ptr<Node>, ObjIdHash> objects_;
};

}  // namespace lnfs::backend
