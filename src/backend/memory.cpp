#include "backend/memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

namespace lnfs::backend {
namespace {

bool valid_component(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

int64_t verf_to_i64(const ExclVerf& verf) {
  int64_t out = 0;
  std::memcpy(&out, verf.data(), sizeof(out));
  return out;
}

}  // namespace

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

  rt::Task<Result<Attr>> setattr(const Cred& cred, const SetAttr& s) override {
    std::lock_guard lock(backend_.mu_);
    Attr& a = node_->attr;
    bool owner = cred.uid == 0 || cred.uid == a.uid;
    if ((s.mode || s.uid || s.gid || s.atime_how != SetAttr::TimeHow::kOmit ||
         s.mtime_how != SetAttr::TimeHow::kOmit) &&
        !owner)
      co_return Err(errno_from(EPERM));
    if (s.size) {
      if (a.type == FType::kDir) co_return Err(errno_from(EISDIR));
      if (a.type != FType::kReg) co_return Err(errno_from(EINVAL));
      if (!owner) {
        uint32_t shift = cred.uid == a.uid ? 6 : (cred.in_group(a.gid) ? 3 : 0);
        if (!((a.mode >> shift) & 2)) co_return Err(errno_from(EACCES));
      }
      node_->data.resize(*s.size);
      a.size = *s.size;
      a.used = node_->data.size();
      a.mtime = backend_.now();
    }
    if (s.mode) a.mode = *s.mode & 07777;
    if (s.uid) a.uid = *s.uid;
    if (s.gid) a.gid = *s.gid;
    auto apply_time = [&](SetAttr::TimeHow how, const Timespec& value, Timespec& out) {
      if (how == SetAttr::TimeHow::kServer) out = backend_.now();
      else if (how == SetAttr::TimeHow::kClient) out = value;
    };
    apply_time(s.atime_how, s.atime, a.atime);
    apply_time(s.mtime_how, s.mtime, a.mtime);
    a.ctime = backend_.now();
    a.change = static_cast<uint64_t>(a.ctime.sec);
    co_return a;
  }

  rt::Task<Result<Created>> create(const Cred& cred, std::string_view name,
                                   const SetAttr& attrs, ExclVerf* verf) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(name)) co_return Err(errno_from(EINVAL));
    std::lock_guard lock(backend_.mu_);
    if (auto it = node_->children.find(name); it != node_->children.end()) {
      // EXCLUSIVE replay: the verifier persisted in atime matches -> same request.
      if (verf && it->second.node->attr.type == FType::kReg &&
          it->second.node->attr.atime.sec == verf_to_i64(*verf))
        co_return Created{backend_.wrap(it->second.node), it->second.node->attr};
      co_return Err(errno_from(EEXIST));
    }
    auto node = backend_.new_child(node_, std::string(name), FType::kReg,
                                   verf ? 0 : attrs.mode.value_or(0644), cred);
    if (verf) {
      node->attr.atime.sec = verf_to_i64(*verf);
    } else if (attrs.size) {
      node->data.resize(*attrs.size);
      node->attr.size = *attrs.size;
    }
    co_return Created{backend_.wrap(node), node->attr};
  }

  rt::Task<Result<Created>> mkdir(const Cred& cred, std::string_view name,
                                  const SetAttr& attrs) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(name)) co_return Err(errno_from(EINVAL));
    std::lock_guard lock(backend_.mu_);
    if (node_->children.contains(name)) co_return Err(errno_from(EEXIST));
    auto node = backend_.new_child(node_, std::string(name), FType::kDir,
                                   attrs.mode.value_or(0755), cred);
    node_->attr.nlink++;
    co_return Created{backend_.wrap(node), node->attr};
  }

  rt::Task<Result<Created>> symlink(const Cred& cred, std::string_view name,
                                    std::string_view target, const SetAttr&) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(name)) co_return Err(errno_from(EINVAL));
    std::lock_guard lock(backend_.mu_);
    if (node_->children.contains(name)) co_return Err(errno_from(EEXIST));
    auto node = backend_.new_child(node_, std::string(name), FType::kLnk, 0777, cred);
    node->link = target;
    node->attr.size = target.size();
    co_return Created{backend_.wrap(node), node->attr};
  }

  rt::Task<Result<Created>> mknod(const Cred& cred, std::string_view name, FType ftype,
                                  DevT dev, const SetAttr& attrs) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(name)) co_return Err(errno_from(EINVAL));
    if (ftype != FType::kChr && ftype != FType::kBlk && ftype != FType::kSock &&
        ftype != FType::kFifo)
      co_return Err(errno_from(EINVAL));
    std::lock_guard lock(backend_.mu_);
    if (node_->children.contains(name)) co_return Err(errno_from(EEXIST));
    auto node = backend_.new_child(node_, std::string(name), ftype,
                                   attrs.mode.value_or(0644), cred);
    node->attr.rdev = dev;
    co_return Created{backend_.wrap(node), node->attr};
  }

  rt::Task<Result<void>> unlink(const Cred&, std::string_view name) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    std::lock_guard lock(backend_.mu_);
    auto it = node_->children.find(name);
    if (it == node_->children.end()) co_return Err(errno_from(ENOENT));
    if (it->second.node->attr.type == FType::kDir) co_return Err(errno_from(EISDIR));
    it->second.node->attr.nlink--;
    it->second.node->attr.ctime = backend_.now();
    backend_.erase_child(node_, name);
    co_return Result<void>{};
  }

  rt::Task<Result<void>> rmdir(const Cred&, std::string_view name) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    std::lock_guard lock(backend_.mu_);
    auto it = node_->children.find(name);
    if (it == node_->children.end()) co_return Err(errno_from(ENOENT));
    if (it->second.node->attr.type != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!it->second.node->children.empty()) co_return Err(errno_from(ENOTEMPTY));
    node_->attr.nlink--;
    backend_.erase_child(node_, name);
    co_return Result<void>{};
  }

  rt::Task<Result<void>> rename(const Cred&, std::string_view from, Object& dst_dir,
                                std::string_view to) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(to)) co_return Err(errno_from(EINVAL));
    auto* dst = dynamic_cast<MemoryObject*>(&dst_dir);
    if (!dst || &dst->backend_ != &backend_) co_return Err(errno_from(EXDEV));
    if (dst->type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    std::lock_guard lock(backend_.mu_);
    auto src_it = node_->children.find(from);
    if (src_it == node_->children.end()) co_return Err(errno_from(ENOENT));
    auto moving = src_it->second.node;
    // Moving a directory under its own descendant would detach the subtree.
    for (auto probe = dst->node_; probe; probe = probe->parent.lock()) {
      if (probe == moving) co_return Err(errno_from(EINVAL));
      if (probe == backend_.root_) break;
    }
    auto dst_it = dst->node_->children.find(to);
    if (dst_it != dst->node_->children.end()) {
      auto existing = dst_it->second.node;
      if (existing == moving) co_return Result<void>{};  // same link: POSIX no-op
      bool src_dir = moving->attr.type == FType::kDir;
      bool dst_is_dir = existing->attr.type == FType::kDir;
      if (src_dir && !dst_is_dir) co_return Err(errno_from(ENOTDIR));
      if (!src_dir && dst_is_dir) co_return Err(errno_from(EISDIR));
      if (dst_is_dir && !existing->children.empty()) co_return Err(errno_from(ENOTEMPTY));
      if (dst_is_dir) dst->node_->attr.nlink--;
      backend_.erase_child(dst->node_, to);
    }
    backend_.erase_child(node_, from);
    if (moving->attr.type == FType::kDir) {
      node_->attr.nlink--;
      dst->node_->attr.nlink++;
    }
    uint64_t cookie = backend_.next_cookie_++;
    dst->node_->children.emplace(std::string(to), Node::Child{moving, cookie});
    dst->node_->cookie_order.emplace(cookie, std::string(to));
    moving->parent = dst->node_;
    moving->attr.ctime = backend_.now();
    co_return Result<void>{};
  }

  rt::Task<Result<void>> link(const Cred&, Object& file, std::string_view name) override {
    if (type() != FType::kDir) co_return Err(errno_from(ENOTDIR));
    if (!valid_component(name)) co_return Err(errno_from(EINVAL));
    auto* target = dynamic_cast<MemoryObject*>(&file);
    if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
    if (target->type() == FType::kDir) co_return Err(errno_from(EPERM));
    std::lock_guard lock(backend_.mu_);
    if (node_->children.contains(name)) co_return Err(errno_from(EEXIST));
    uint64_t cookie = backend_.next_cookie_++;
    node_->children.emplace(std::string(name), Node::Child{target->node_, cookie});
    node_->cookie_order.emplace(cookie, std::string(name));
    target->node_->attr.nlink++;
    target->node_->attr.ctime = backend_.now();
    node_->attr.mtime = node_->attr.ctime = backend_.now();
    co_return Result<void>{};
  }

  rt::Task<Result<uint32_t>> write(OpenCtx ctx, uint64_t off, std::span<const std::byte> in,
                                   Stability) override {
    if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
    if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
    auto allowed = co_await access(ctx.cred, Access::kModify);
    if (!allowed || (!allowed->has(Access::kModify) && ctx.cred.uid != node_->attr.uid))
      co_return Err(allowed ? errno_from(EACCES) : allowed.error());
    std::lock_guard lock(backend_.mu_);
    if (off + in.size() > node_->data.size()) node_->data.resize(off + in.size());
    std::copy(in.begin(), in.end(), node_->data.begin() + static_cast<size_t>(off));
    node_->attr.size = node_->data.size();
    node_->attr.used = node_->data.size();
    node_->attr.mtime = node_->attr.ctime = backend_.now();
    node_->attr.change = static_cast<uint64_t>(node_->attr.ctime.sec);
    co_return static_cast<uint32_t>(in.size());
  }

  rt::Task<Result<void>> commit(OpenCtx, uint64_t, uint64_t) override {
    co_return Result<void>{};  // memory is as stable as it gets
  }

  // ---- v4.2 sweets (design 05 §5.x: kSparseOps / kCopyRange / kCloneRange) ----
  // A byte vector has no holes: SEEK treats every byte inside [0,size) as data and the
  // implicit hole at EOF as the only hole, exactly what lseek(2) reports for a dense
  // file.  DEALLOCATE zeroes (never shrinks); ALLOCATE extends with zeroes.

  rt::Task<Result<uint64_t>> seek(OpenCtx ctx, uint64_t off, SeekWhat what) override {
    auto gate = co_await io_gate(ctx, /*write=*/false);
    if (!gate) co_return Err(gate.error());
    std::lock_guard lock(backend_.mu_);
    uint64_t size = node_->data.size();
    if (off >= size) co_return Err(errno_from(ENXIO));
    co_return what == SeekWhat::kData ? off : size;
  }

  rt::Task<Result<void>> allocate(OpenCtx ctx, uint64_t off, uint64_t len) override {
    auto gate = co_await io_gate(ctx, /*write=*/true);
    if (!gate) co_return Err(gate.error());
    std::lock_guard lock(backend_.mu_);
    if (off + len > node_->data.size()) {
      node_->data.resize(static_cast<size_t>(off + len));
      touch();
    }
    co_return Result<void>{};
  }

  rt::Task<Result<void>> deallocate(OpenCtx ctx, uint64_t off, uint64_t len) override {
    auto gate = co_await io_gate(ctx, /*write=*/true);
    if (!gate) co_return Err(gate.error());
    std::lock_guard lock(backend_.mu_);
    uint64_t size = node_->data.size();
    if (off < size) {
      uint64_t end = std::min<uint64_t>(size, off + len);
      std::fill(node_->data.begin() + static_cast<size_t>(off),
                node_->data.begin() + static_cast<size_t>(end), std::byte{0});
      touch();
    }
    co_return Result<void>{};
  }

  rt::Task<Result<void>> clone(OpenCtx sctx, Object& dst, OpenCtx dctx, uint64_t soff,
                               uint64_t doff, uint64_t len) override {
    auto copied = co_await copy_range(sctx, dst, dctx, soff, doff, len);
    if (!copied) co_return Err(copied.error());
    co_return Result<void>{};
  }

  rt::Task<Result<uint64_t>> copy_range(OpenCtx sctx, Object& dst, OpenCtx dctx,
                                        uint64_t soff, uint64_t doff, uint64_t len) override {
    auto* target = dynamic_cast<MemoryObject*>(&dst);
    if (!target || &target->backend_ != &backend_) co_return Err(errno_from(EXDEV));
    auto sgate = co_await io_gate(sctx, false);
    if (!sgate) co_return Err(sgate.error());
    auto dgate = co_await target->io_gate(dctx, true);
    if (!dgate) co_return Err(dgate.error());
    std::lock_guard lock(backend_.mu_);
    uint64_t ssize = node_->data.size();
    if (soff >= ssize) co_return 0;  // nothing to copy at/after EOF
    uint64_t n = std::min<uint64_t>(len, ssize - soff);
    std::vector<std::byte> chunk(node_->data.begin() + static_cast<size_t>(soff),
                                 node_->data.begin() + static_cast<size_t>(soff + n));
    auto& out = target->node_->data;
    if (doff + n > out.size()) out.resize(static_cast<size_t>(doff + n));
    std::copy(chunk.begin(), chunk.end(), out.begin() + static_cast<size_t>(doff));
    target->touch();
    co_return n;
  }

 private:
  // Regular-file precondition + permission gate shared by the v4.2 ops (same owner
  // relaxation as read/write).
  rt::Task<Result<void>> io_gate(OpenCtx ctx, bool write) {
    if (type() == FType::kDir) co_return Err(errno_from(EISDIR));
    if (type() != FType::kReg) co_return Err(errno_from(EINVAL));
    auto allowed = co_await access(ctx.cred, write ? Access::kModify : Access::kRead);
    if (!allowed) co_return Err(allowed.error());
    if (!allowed->has(write ? Access::kModify : Access::kRead) &&
        ctx.cred.uid != node_->attr.uid)
      co_return Err(errno_from(EACCES));
    co_return Result<void>{};
  }
  void touch() {  // callers hold backend_.mu_
    node_->attr.size = node_->data.size();
    node_->attr.used = node_->data.size();
    node_->attr.mtime = node_->attr.ctime = backend_.now();
    node_->attr.change = static_cast<uint64_t>(node_->attr.ctime.sec);
  }

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
  return out.set(Cap::kSymlink).set(Cap::kHardlink).set(Cap::kMknod)
      .set(Cap::kStableHandles).set(Cap::kSparseOps).set(Cap::kCopyRange)
      .set(Cap::kCloneRange);
}

Timespec MemoryBackend::now() { return Timespec{tick_++, 0}; }

std::shared_ptr<MemoryBackend::Node> MemoryBackend::new_child(
    const std::shared_ptr<Node>& parent, std::string name, FType type, uint32_t mode,
    const Cred& cred) {
  auto node = std::make_shared<Node>();
  node->id = next_id_++;
  node->attr.type = type;
  node->attr.mode = mode & 07777;
  node->attr.nlink = type == FType::kDir ? 2 : 1;
  node->attr.uid = cred.uid;
  node->attr.gid = cred.gid;
  node->attr.fileid = node->id;
  node->attr.atime = node->attr.mtime = node->attr.ctime = now();
  node->attr.change = static_cast<uint64_t>(node->attr.ctime.sec);
  node->parent = parent;
  uint64_t cookie = next_cookie_++;
  parent->children.emplace(name, Node::Child{node, cookie});
  parent->cookie_order.emplace(cookie, std::move(name));
  parent->attr.mtime = parent->attr.ctime = now();
  parent->attr.change = static_cast<uint64_t>(parent->attr.ctime.sec);
  objects_[id_for(node->id)] = node;
  return node;
}

void MemoryBackend::erase_child(const std::shared_ptr<Node>& parent, std::string_view name) {
  auto it = parent->children.find(name);
  if (it == parent->children.end()) return;
  parent->cookie_order.erase(it->second.cookie);
  parent->children.erase(it);
  parent->attr.mtime = parent->attr.ctime = now();
  parent->attr.change = static_cast<uint64_t>(parent->attr.ctime.sec);
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
