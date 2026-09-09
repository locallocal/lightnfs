#pragma once
// v4 pseudo filesystem (design 04 §4.3/5.2, nfsv4 research 03 §3.1): a synthesized
// read-only directory tree from the pseudo root ("/") down to each export point.
// fsid 0 is reserved for pseudo nodes; crossing into an export switches to the
// export's own fsid/backend.  Not served to v3 (which mounts export paths directly).

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.hpp"

namespace lnfs::core {

class PseudoFs {
 public:
  struct Node {
    uint64_t id = 0;  // fileid and ObjId payload
    std::string name;
    Node* parent = nullptr;
    std::map<std::string, std::unique_ptr<Node>, std::less<>> children;
    ExportEntry* exp = nullptr;  // set: this node crosses into that export
  };

  // Built over an ExportSet's entries by ExportSetBuilder::finish (plan 12 B1) and
  // owned by that set; nodes keep raw pointers into `entries`.  `boot_epoch` feeds the
  // synthesized change attribute so clients revalidate the pseudo tree after a
  // restart/reconfig (plan doc 10 §1.6).
  explicit PseudoFs(const std::vector<std::shared_ptr<ExportEntry>>& entries,
                    uint64_t boot_epoch = 1);

  // Nodes are handed out non-const like find()/for_export(): the tree is frozen with
  // its ExportSet, but engines walk it through mutable Node pointers.
  Node* root() const { return const_cast<Node*>(&root_); }
  Node* find(uint64_t id) const;
  Node* for_export(uint32_t fsid) const;  // pseudo node crossing into fsid (or null)

  backend::Attr attr_of(const Node& node) const;
  static backend::ObjId oid_of(const Node& node);
  // The node's path from the pseudo root as pathname4 components (empty for the root):
  // the fs_root of an export's fs_locations (plan 12 B2).
  static std::vector<std::string> path_of(const Node& node);
  // Reverses oid_of; null if the id does not name a live pseudo node.
  Node* resolve(const backend::ObjId& oid) const;

 private:
  Node* ensure_child(Node* parent, std::string_view name, std::string_view full_path);
  // Node id derived from the node's pseudo path (plan doc 10 §1.6): stable across
  // restarts and export-set changes, so an old pseudo filehandle either resolves to
  // the same directory or goes cleanly stale — never silently to a different node.
  uint64_t stable_id(std::string_view path);

  Node root_;
  std::unordered_map<uint64_t, Node*> by_id_;
  std::unordered_map<uint32_t, Node*> by_export_;
  uint64_t boot_epoch_ = 1;
};

}  // namespace lnfs::core
