#include "core/pseudofs.hpp"

#include <cstring>

namespace lnfs::core {

namespace {

uint64_t fnv64(std::string_view bytes) {
  uint64_t h = 1469598103934665603ull;
  for (char c : bytes) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace

uint64_t PseudoFs::stable_id(std::string_view path) {
  // Path-hash ids replace the construction-order counter (plan doc 10 §1.6): the old
  // scheme made a restart with a changed export set silently re-point old pseudo
  // filehandles at different nodes.  Collisions are probed away; the probe order is a
  // theoretical (1-in-2^64) determinism caveat, not a practical one.
  uint64_t id = fnv64(path);
  while (id == 0 || by_id_.contains(id)) ++id;
  return id;
}

PseudoFs::PseudoFs(const ExportTable& exports, uint64_t boot_epoch)
    : boot_epoch_(boot_epoch) {
  root_.id = stable_id("/");
  root_.name = "/";
  by_id_[root_.id] = &root_;
  for (const auto& entry : exports.entries()) {
    Node* cur = &root_;
    std::string_view path = entry->path;
    std::string full;
    size_t pos = 0;
    while (pos < path.size() && path[pos] == '/') ++pos;
    while (pos < path.size()) {
      size_t slash = path.find('/', pos);
      std::string_view part =
          path.substr(pos, slash == std::string_view::npos ? path.size() - pos : slash - pos);
      if (!part.empty()) {
        full += '/';
        full += part;
        cur = ensure_child(cur, part, full);
      }
      if (slash == std::string_view::npos) break;
      pos = slash + 1;
    }
    // Nested exports under another export's subtree stay v3-only: the outer crossing
    // wins in the v4 namespace (documented limitation).
    if (!cur->exp) {
      cur->exp = entry.get();
      by_export_[entry->fsid] = cur;
    }
  }
}

PseudoFs::Node* PseudoFs::ensure_child(Node* parent, std::string_view name,
                                       std::string_view full_path) {
  auto it = parent->children.find(name);
  if (it != parent->children.end()) return it->second.get();
  auto node = std::make_unique<Node>();
  node->id = stable_id(full_path);
  node->name = std::string(name);
  node->parent = parent;
  Node* raw = node.get();
  by_id_[raw->id] = raw;
  parent->children.emplace(std::string(name), std::move(node));
  return raw;
}

PseudoFs::Node* PseudoFs::find(uint64_t id) const {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

PseudoFs::Node* PseudoFs::for_export(uint32_t fsid) const {
  auto it = by_export_.find(fsid);
  return it == by_export_.end() ? nullptr : it->second;
}

backend::Attr PseudoFs::attr_of(const Node& node) const {
  backend::Attr a;
  a.type = backend::FType::kDir;
  a.mode = 0555;  // pseudo directories are read-only by construction
  a.nlink = 2 + static_cast<uint32_t>(node.children.size());
  a.uid = 0;
  a.gid = 0;
  a.size = 4096;
  a.used = 4096;
  a.fileid = node.id;
  // The synthesized tree only changes on restart/reconfig — which is exactly when the
  // boot epoch moves, so client caches revalidate then (plan doc 10 §1.6).
  a.change = boot_epoch_;
  return a;
}

backend::ObjId PseudoFs::oid_of(const Node& node) {
  backend::ObjId out;
  out.len = 8;
  std::memcpy(out.bytes.data(), &node.id, sizeof(node.id));
  return out;
}

PseudoFs::Node* PseudoFs::resolve(const backend::ObjId& oid) const {
  if (oid.len != 8) return nullptr;
  uint64_t id = 0;
  std::memcpy(&id, oid.bytes.data(), sizeof(id));
  return find(id);
}

}  // namespace lnfs::core
