#pragma once
// In-memory ClusterStore for the cluster tests (controller, ctl `cluster *`): the
// fence can be aged or handed to "another node", renew/read failures can be injected,
// and every mutating call is logged in order.

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

  void age_out() { fence->expires_at_ms = wall_now_ms() - 10000; }
  void taken_by(const std::string& other, uint64_t e) {
    fence = server::FenceRecord{other, e, wall_now_ms() + 60000};
  }
};

}  // namespace lnfs::test
