#pragma once
// Shared cluster state (design 09 §9.4, plan 10 A2): the handle key, the global boot
// epoch, the v4 reclaim list, the fence lease and the per-node export digests that
// every gateway of one cluster must see.  The interface is what the state manager and
// the cluster controller program against; the POSIX implementation keeps it as files
// on a shared filesystem (a cluster-backend mount, or any shared directory), written
// with core::atomic_write_file so a reader never sees a torn record.
//
// Every call blocks on filesystem IO: use from the main thread or an offload thread,
// never on a reactor.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/result.hpp"

namespace lnfs::server {

// Layout under shared_dir (all files replaced atomically):
//   hmac.key            handle HMAC key: first gateway creates it, the rest read it
//   epoch               global boot epoch, +1 per takeover (not per process start)
//   clients/<fnv64>     reclaim list: co_ownerid verbatim (same naming as state_dir/)
//   fence               "<epoch> <expires_at_unix_ms> <node>\n"
//   exports.<node>      canonical export-table digest of that node
//   epoch.lock / fence.lock   O_EXCL serialization of the two multi-writer files
struct FenceRecord {
  std::string node;
  uint64_t epoch = 0;
  int64_t expires_at_ms = 0;  // wall clock (CLOCK_REALTIME); gateways must run NTP
};

class ClusterStore {
 public:
  virtual ~ClusterStore() = default;

  virtual Result<std::array<std::byte, 16>> load_or_create_key() = 0;

  // Epoch: read_epoch answers 0 before the first bump; bump_epoch is serialized.
  virtual Result<uint64_t> read_epoch() = 0;
  virtual Result<uint64_t> bump_epoch() = 0;

  // Reclaim list: owner ids verbatim; put is idempotent, erase of a missing id is ok.
  virtual Result<std::vector<std::string>> list_clients() = 0;
  virtual Result<void> put_client(std::string_view owner_id) = 0;
  virtual Result<void> erase_client(std::string_view owner_id) = 0;

  // Fence lease.  acquire rewrites the record as {node, epoch, now + ttl} only when
  // there is none, it has expired (with clock-skew tolerance), it already belongs to
  // `node`, or `force` is set; otherwise EBUSY and the current holder is left alone.
  // renew extends our own record (EPERM when the record is missing or someone else's,
  // the signal for an active gateway to drain).  release removes our own record
  // (EPERM for someone else's; ok when there is none).
  virtual Result<std::optional<FenceRecord>> read_fence() = 0;
  virtual Result<FenceRecord> acquire_fence(std::string_view node, uint64_t epoch,
                                            std::chrono::milliseconds ttl, bool force) = 0;
  virtual Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) = 0;
  virtual Result<void> release_fence(std::string_view node) = 0;

  // Export-table digests: one record per node, overwritten on every start.
  virtual Result<void> put_exports_digest(std::string_view node, std::string_view digest) = 0;
  virtual Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() = 0;
};

// Clock-skew allowance applied to fence expiry checks.
inline constexpr std::chrono::milliseconds kFenceSkewTolerance{500};

// The POSIX-directory implementation.  `stale_lock_after`: a `.lock` file older than
// this belongs to a dead writer and is reclaimed (2 × fence_lease in the controller).
// The directory (and clients/) is created on first use.
std::unique_ptr<ClusterStore> make_posix_cluster_store(
    std::string shared_dir, std::chrono::milliseconds stale_lock_after = std::chrono::seconds(6));

}  // namespace lnfs::server
