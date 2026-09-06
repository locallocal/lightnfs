#pragma once
// Per-export ownership as this gateway sees it (design 11 §11.3/§11.4, plan 12 B2):
// the cluster controller publishes a fresh snapshot whenever a fence, an owner record
// or one of its own roles changes; the v4 engine reads one snapshot per operation to
// decide between serving an export, referring the client to its owner (fs_locations /
// NFS4ERR_MOVED) or asking it to wait.  An empty view — a single gateway, a failover
// gateway — means every export is served here, so nothing changes outside
// active-active.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lnfs::core {

enum class FsRole : uint8_t {
  kActive,    // this gateway owns the export and serves it
  kDraining,  // this gateway is handing the export over: refer, do not serve
  kRemote,    // another gateway owns it: refer there
  kUnowned,   // nobody holds its fence right now: the client should retry
};

const char* fs_role_name(FsRole role);

struct FsOwner {
  FsRole role = FsRole::kActive;
  std::string node;     // the owner (this node when kActive / kDraining)
  std::string address;  // the owner's `[cluster] node_address`; empty when unknown
  uint64_t fs_epoch = 0;
};

class FsOwnerView {
 public:
  using Map = std::unordered_map<uint32_t, FsOwner>;

  // The current snapshot; never null (an empty map before the first publish).
  std::shared_ptr<const Map> snapshot() const { return current_.load(std::memory_order_acquire); }
  void publish(Map owners) {
    current_.store(std::make_shared<const Map>(std::move(owners)), std::memory_order_release);
  }

 private:
  std::atomic<std::shared_ptr<const Map>> current_{std::make_shared<const Map>()};
};

// The host part of a "host:port" / "[v6]:port" node address — what fs_locations
// carries: RFC 8881 §11.10 servers are names or address literals, and clients (Linux
// included) reach a referral target on the NFS port.
std::string address_host(std::string_view address);

inline const char* fs_role_name(FsRole role) {
  switch (role) {
    case FsRole::kActive: return "active";
    case FsRole::kDraining: return "draining";
    case FsRole::kRemote: return "remote";
    case FsRole::kUnowned: return "unowned";
  }
  return "?";
}

inline std::string address_host(std::string_view address) {
  size_t colon = address.rfind(':');
  std::string_view host = colon == std::string_view::npos ? address : address.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  return std::string(host);
}

}  // namespace lnfs::core
