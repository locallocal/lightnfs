#include "backend/memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

namespace lnfs::backend {

struct MemoryBackend::Node {
  struct Child {
    std::shared_ptr<Node> node;
    uint64_t cookie;
  };
  uint64_t id = 0;
  Attr attr;
  std::vector<std::byte> data;
  std::string link;
  std::map<std::string, Child, std::less<>> children;
  std::map<uint64_t, std::string> cookie_order;
  std::weak_ptr<Node> parent;
};

class MemoryBackend::MemoryObject final : public Object {
 public:
  MemoryObject(MemoryBackend& backend, std::shared_ptr<Node> node)
      : Object(backend.id_for(node->id), node->attr.type), backend_(backend), node_(std::move(node)) {}

  rt::Task<Result<Attr>> getattr() override {
    std::lock_guard lock(backend_.mu_);
    co_return node_->attr;
  }

  rt::Task<Result<ObjPtr>> lookup(const Cred& cred, std::string_view name) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    auto allowed = co_await access(cred, Access::kLookup);
    if (!allowed || !allowed->has(Access::kLookup))
      co_return Err(allowed ? errno_from(EACCES) : allowed.error());
    std::lock_guard lock(backend_.mu_);
    if (name == ".") co_return backend_.wrap(node_);
    if (name == "..") {
      auto parent = node_->parent.lock();
      co_return backend_.wrap(parent ? parent : node_);
    }
    auto it = node_->children.find(name);
    if (it == node_->children.end()) co_return Err(errno_from(ENOENT));
    co_return backend_.wrap(it->second.node);
  }

  rt::Task<Result<DirPage>> readdir(const Cred& cred, uint64_t cookie,
                                     uint32_t max_entries) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    auto allowed = co_await access(cred, Access::kRead);
    if (!allowed || !allowed->has(Access::kRead))
      co_return Err(allowed ? errno_from(EACCES) : allowed.error());
    DirPage page;
    std::lock_guard lock(backend_.mu_);
    for (auto pos = node_->cookie_order.upper_bound(cookie);
         pos != node_->cookie_order.end(); ++pos) {
      const auto& name = pos->second;
      const auto& child = node_->children.find(name)->second;
      page.ents.push_back({.name = name,
                           .cookie = child.cookie,
                           .fileid = child.node->attr.fileid,
                           .attr = child.node->attr,
                           .oid = backend_.id_for(child.node->id)});
      if (page.ents.size() >= max_entries) break;
    }
    page.eof = page.ents.empty() ||
               page.ents.back().cookie ==
                   (node_->cookie_order.empty() ? 0 : node_->cookie_order.rbegin()->first);
    co_return page;
  }

  rt::Task<Result<std::string>> readlink() override {
    if (type() != FType::kLnk) co_return Err(errno_from(EINVAL));
    std::lock_guard lock(backend_.mu_);
    co_return node_->link;
  }

  rt::Task<Result<uint32_t>> read(OpenCtx ctx, uint64_t off, std::span<std::byte> out,
                                  bool& eof) override {
    if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
    if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
    auto allowed = co_await access(ctx.cred, Access::kRead);
    if (!allowed || (!allowed->has(Access::kRead) && ctx.cred.uid != node_->attr.uid))
      co_return Err(allowed ? errno_from(EACCES) : allowed.error());
    std::lock_guard lock(backend_.mu_);
    size_t pos = static_cast<size_t>(std::min<uint64_t>(off, node_->data.size()));
    size_t n = std::min(out.size(), node_->data.size() - pos);
    std::copy_n(node_->data.data() + pos, n, out.data());
    eof = pos + n == node_->data.size();
    co_return static_cast<uint32_t>(n);
  }

 private:
  MemoryBackend& backend_;
  std::shared_ptr<Node> node_;
};

MemoryBackend::MemoryBackend(uint64_t fsid) : fsid_(fsid), root_(std::make_shared<Node>()) {
  root_->id = 1;
  root_->attr.type = FType::kDir;
  root_->attr.mode = 0755;
  root_->attr.nlink = 2;
  root_->attr.fileid = 1;
  objects_[id_for(1)] = root_;
}

Caps MemoryBackend::caps() const {
  Caps out;
  return out.set(Cap::kSymlink).set(Cap::kHardlink).set(Cap::kStableHandles);
}

ObjId MemoryBackend::id_for(uint64_t id) const {
  ObjId out;
  out.len = 8;
  std::memcpy(out.bytes.data(), &id, sizeof(id));
  return out;
}

ObjPtr MemoryBackend::wrap(const std::shared_ptr<Node>& node) {
  return std::static_pointer_cast<Object>(std::make_shared<MemoryObject>(*this, node));
}

rt::Task<Result<ObjPtr>> MemoryBackend::root() { co_return wrap(root_); }

rt::Task<Result<ObjPtr>> MemoryBackend::resolve(const ObjId& oid) {
  std::lock_guard lock(mu_);
  auto it = objects_.find(oid);
  if (it == objects_.end()) co_return Err(errno_from(ESTALE));
  auto node = it->second.lock();
  if (!node) co_return Err(errno_from(ESTALE));
  co_return wrap(node);
}

rt::Task<Result<FsStats>> MemoryBackend::statfs() {
  std::lock_guard lock(mu_);
  FsStats out;
  out.tbytes = 1ull << 40;
  uint64_t used = 0;
  for (const auto& [_, weak] : objects_) {
    if (auto n = weak.lock()) used += n->data.size();
  }
  out.fbytes = out.abytes = out.tbytes - used;
  out.tfiles = 1ull << 32;
  out.ffiles = out.afiles = out.tfiles - objects_.size();
  co_return out;
}

std::shared_ptr<MemoryBackend::Node> MemoryBackend::find_path(std::string_view path) {
  auto cur = root_;
  size_t pos = 0;
  while (pos < path.size() && path[pos] == '/') ++pos;
  while (pos < path.size()) {
    size_t slash = path.find('/', pos);
    std::string_view part = path.substr(pos, slash == std::string_view::npos ? path.size() - pos
                                                                            : slash - pos);
    if (part.empty() || part == ".") {
      // no-op
    } else {
      auto it = cur->children.find(part);
      if (it == cur->children.end() || it->second.node->attr.type != FType::kDir) return {};
      cur = it->second.node;
    }
    if (slash == std::string_view::npos) break;
    pos = slash + 1;
  }
  return cur;
}

Result<ObjId> MemoryBackend::add_node(std::string_view path, FType type, uint32_t mode,
                                      std::span<const std::byte> data, std::string_view link) {
  std::lock_guard lock(mu_);
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  size_t slash = path.rfind('/');
  std::string_view parent_path = slash == std::string_view::npos ? std::string_view("/")
                                                                  : path.substr(0, slash);
  std::string name(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
    return Err(errno_from(EINVAL));
  auto parent = find_path(parent_path);
  if (!parent) return Err(errno_from(ENOENT));
  if (parent->children.contains(name)) return Err(errno_from(EEXIST));
  auto node = std::make_shared<Node>();
  node->id = next_id_++;
  node->attr.type = type;
  node->attr.mode = mode;
  node->attr.nlink = type == FType::kDir ? 2 : 1;
  node->attr.fileid = node->id;
  node->attr.size = type == FType::kLnk ? link.size() : data.size();
  node->attr.used = data.size();
  node->data.assign(data.begin(), data.end());
  node->link = link;
  node->parent = parent;
  uint64_t cookie = next_cookie_++;
  parent->children.emplace(name, Node::Child{node, cookie});
  parent->cookie_order.emplace(cookie, name);
  objects_[id_for(node->id)] = node;
  return id_for(node->id);
}

Result<ObjId> MemoryBackend::add_dir(std::string_view path, uint32_t mode) {
  return add_node(path, FType::kDir, mode, {}, {});
}
Result<ObjId> MemoryBackend::add_file(std::string_view path, std::span<const std::byte> data,
                                      uint32_t mode) {
  return add_node(path, FType::kReg, mode, data, {});
}
Result<ObjId> MemoryBackend::add_symlink(std::string_view path, std::string_view target) {
  return add_node(path, FType::kLnk, 0777, {}, target);
}

}  // namespace lnfs::backend
