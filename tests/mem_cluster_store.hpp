#pragma once
// In-memory ClusterStore for the cluster tests (controller, ctl `cluster *`): the
// fence can be aged or handed to "another node", renew/read failures can be injected,
// and every mutating call is logged in order.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "server/cluster_store.hpp"

namespace lnfs::test {

inline int64_t wall_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct MemClusterStore final : server::ClusterStore {
  uint64_t epoch = 0;
  std::optional<server::FenceRecord> fence;
  std::map<std::string, std::string> clients, digests;
  std::vector<std::string> log;
  Errno fail_renew = Errno::kOk;  // IO trouble injected into renew_fence
  Errno fail_read = Errno::kOk;
  Errno fail_list = Errno::kOk;  // injected into list_exports_digests

  Result<std::array<std::byte, 16>> load_or_create_key() override {
    return std::array<std::byte, 16>{std::byte{1}};
  }
  Result<uint64_t> read_epoch() override { return epoch; }
  Result<uint64_t> bump_epoch() override {
    log.push_back("epoch");
    return ++epoch;
  }
  Result<std::vector<std::string>> list_clients() override {
    std::vector<std::string> out;
    for (auto& [k, v] : clients) out.push_back(v);
    return out;
  }
  Result<void> put_client(std::string_view o) override {
    clients[std::string(o)] = std::string(o);
    return {};
  }
  Result<void> erase_client(std::string_view o) override {
    clients.erase(std::string(o));
    return {};
  }
  Result<std::optional<server::FenceRecord>> read_fence() override {
    if (fail_read != Errno::kOk) return Err(fail_read);
    return fence;
  }
  Result<server::FenceRecord> acquire_fence(std::string_view node, uint64_t e,
                                            std::chrono::milliseconds ttl,
                                            bool force) override {
    log.push_back(std::string("acquire") + (force ? "!" : ""));
    bool expired =
        fence && wall_now_ms() > fence->expires_at_ms + server::kFenceSkewTolerance.count();
    if (fence && !force && fence->node != node && !expired) return Err(errno_from(EBUSY));
    fence = server::FenceRecord{std::string(node), e, wall_now_ms() + ttl.count()};
    return *fence;
  }
  Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) override {
    log.push_back("renew");
    if (fail_renew != Errno::kOk) return Err(fail_renew);
    if (!fence || fence->node != node) return Err(errno_from(EPERM));
    fence->expires_at_ms = wall_now_ms() + ttl.count();
    return {};
  }
  Result<void> release_fence(std::string_view node) override {
    log.push_back("release");
    if (!fence) return {};
    if (fence->node != node) return Err(errno_from(EPERM));
    fence.reset();
    return {};
  }
  Result<void> put_exports_digest(std::string_view n, std::string_view d) override {
    digests[std::string(n)] = std::string(d);
    return {};
  }
  Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() override {
    if (fail_list != Errno::kOk) return Err(fail_list);
    return std::vector<std::pair<std::string, std::string>>(digests.begin(), digests.end());
  }

  // ---- active-active (plan 12 A2): per-node batched fences, per-fsid records ----
  std::map<std::string, server::NodeFences> fences;  // node → record (expired ones too)
  std::map<std::string, uint64_t> node_epochs;
  std::map<std::string, std::string> node_addresses;
  std::map<uint32_t, uint64_t> fs_epochs;
  std::map<uint32_t, server::OwnerRecord> owners;
  std::map<uint32_t, std::map<std::string, std::string>> fs_clients;

  static bool fences_expired(const server::NodeFences& rec) {
    return wall_now_ms() > rec.expires_at_ms + server::kFenceSkewTolerance.count();
  }
  static const server::NodeFences::Hold* hold_of(const server::NodeFences& rec, uint32_t fsid) {
    for (const auto& h : rec.holds)
      if (h.fsid == fsid) return &h;
    return nullptr;
  }
  static void drop_hold(server::NodeFences& rec, uint32_t fsid) {
    std::erase_if(rec.holds, [&](const auto& h) { return h.fsid == fsid; });
  }

  Result<uint64_t> read_node_epoch(std::string_view node) override {
    auto it = node_epochs.find(std::string(node));
    return it == node_epochs.end() ? 0 : it->second;
  }
  Result<uint64_t> bump_node_epoch(std::string_view node) override {
    log.push_back("node_epoch:" + std::string(node));
    return ++node_epochs[std::string(node)];
  }
  Result<void> put_node_address(std::string_view node, std::string_view address) override {
    node_addresses[std::string(node)] = std::string(address);
    return {};
  }
  Result<std::vector<std::pair<std::string, std::string>>> list_nodes() override {
    if (fail_list != Errno::kOk) return Err(fail_list);
    return std::vector<std::pair<std::string, std::string>>(node_addresses.begin(),
                                                            node_addresses.end());
  }
  Result<std::optional<server::FenceRecord>> read_fs_fence(uint32_t fsid) override {
    if (fail_read != Errno::kOk) return Err(fail_read);
    std::optional<server::FenceRecord> best;
    for (const auto& [node, rec] : fences) {
      const auto* h = hold_of(rec, fsid);
      if (!h) continue;
      server::FenceRecord c{node, h->epoch, rec.expires_at_ms};
      bool best_expired = best && wall_now_ms() > best->expires_at_ms + server::kFenceSkewTolerance.count();
      if (!best || (best_expired && (!fences_expired(rec) || c.expires_at_ms > best->expires_at_ms)))
        best = c;
    }
    return best;
  }
  Result<server::FenceRecord> acquire_fs_fence(uint32_t fsid, std::string_view node, uint64_t e,
                                               std::chrono::milliseconds ttl,
                                               bool force) override {
    log.push_back("acquire_fs:" + std::to_string(fsid) + (force ? "!" : ""));
    for (auto& [other, rec] : fences)
      if (other != node && hold_of(rec, fsid) && !force && !fences_expired(rec))
        return Err(errno_from(EBUSY));
    for (auto& [other, rec] : fences)
      if (other != node) drop_hold(rec, fsid);
    auto& mine = fences[std::string(node)];
    mine.node = std::string(node);
    drop_hold(mine, fsid);
    mine.holds.push_back({fsid, e});
    std::sort(mine.holds.begin(), mine.holds.end(),
              [](const auto& a, const auto& b) { return a.fsid < b.fsid; });
    mine.expires_at_ms = wall_now_ms() + ttl.count();
    return server::FenceRecord{std::string(node), e, mine.expires_at_ms};
  }
  Result<void> renew_fences(std::string_view node, std::chrono::milliseconds ttl) override {
    log.push_back("renew_fences");
    if (fail_renew != Errno::kOk) return Err(fail_renew);
    auto& mine = fences[std::string(node)];
    mine.node = std::string(node);
    mine.expires_at_ms = wall_now_ms() + ttl.count();
    return {};
  }
  Result<void> release_fs_fence(uint32_t fsid, std::string_view node) override {
    log.push_back("release_fs:" + std::to_string(fsid));
    for (auto& [other, rec] : fences) {
      if (!hold_of(rec, fsid)) continue;
      if (other != node) return Err(errno_from(EPERM));
      drop_hold(rec, fsid);
      return {};
    }
    return {};
  }
  Result<std::vector<server::NodeFences>> list_fences() override {
    if (fail_read != Errno::kOk) return Err(fail_read);
    std::vector<server::NodeFences> out;
    for (const auto& [node, rec] : fences) out.push_back(rec);
    return out;
  }
  Result<uint64_t> read_fs_epoch(uint32_t fsid) override {
    auto it = fs_epochs.find(fsid);
    return it == fs_epochs.end() ? 0 : it->second;
  }
  Result<uint64_t> bump_fs_epoch(uint32_t fsid) override {
    log.push_back("fs_epoch:" + std::to_string(fsid));
    return ++fs_epochs[fsid];
  }
  Result<std::optional<server::OwnerRecord>> read_owner(uint32_t fsid) override {
    if (fail_read != Errno::kOk) return Err(fail_read);
    auto it = owners.find(fsid);
    if (it == owners.end()) return std::optional<server::OwnerRecord>{};
    return std::optional<server::OwnerRecord>{it->second};
  }
  Result<void> put_owner(uint32_t fsid, const server::OwnerRecord& owner) override {
    log.push_back("put_owner:" + std::to_string(fsid) + "=" + owner.node);
    owners[fsid] = owner;
    return {};
  }
  Result<std::vector<std::string>> list_clients(uint32_t fsid) override {
    std::vector<std::string> out;
    for (auto& [k, v] : fs_clients[fsid]) out.push_back(v);
    return out;
  }
  Result<void> put_client(uint32_t fsid, std::string_view o) override {
    fs_clients[fsid][std::string(o)] = std::string(o);
    return {};
  }
  Result<void> erase_client(uint32_t fsid, std::string_view o) override {
    fs_clients[fsid].erase(std::string(o));
    return {};
  }

  // Test knobs: age one node's whole record out, or hand an fsid to another node.
  void age_out_node(const std::string& node) { fences[node].expires_at_ms = wall_now_ms() - 10000; }
  void fs_taken_by(uint32_t fsid, const std::string& other, uint64_t e) {
    for (auto& [n, rec] : fences) drop_hold(rec, fsid);
    auto& rec = fences[other];
    rec.node = other;
    rec.holds.push_back({fsid, e});
    rec.expires_at_ms = wall_now_ms() + 60000;
  }

  void age_out() { fence->expires_at_ms = wall_now_ms() - 10000; }
  void taken_by(const std::string& other, uint64_t e) {
    fence = server::FenceRecord{other, e, wall_now_ms() + 60000};
  }
};

}  // namespace lnfs::test
