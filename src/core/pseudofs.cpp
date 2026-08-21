#include "core/pseudofs.hpp"

#include <cstring>

namespace lnfs::core {

PseudoFs::PseudoFs(const ExportTable& exports) {
  root_.id = next_id_++;
  root_.name = "/";
  by_id_[root_.id] = &root_;
  for (const auto& entry : exports.entries()) {
    Node* cur = &root_;
    std::string_view path = entry->path;
    size_t pos = 0;
    while (pos < path.size() && path[pos] == '/') ++pos;
    while (pos < path.size()) {
      size_t slash = path.find('/', pos);
      std::string_view part =
          path.substr(pos, slash == std::string_view::npos ? path.size() - pos : slash - pos);
      if (!part.empty()) cur = ensure_child(cur, part);
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

PseudoFs::Node* PseudoFs::ensure_child(Node* parent, std::string_view name) {
  auto it = parent->children.find(name);
  if (it != parent->children.end()) return it->second.get();
  auto node = std::make_unique<Node>();
  node->id = next_id_++;
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
  a.change = 1;  // synthesized tree only changes on restart/reconfig
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
