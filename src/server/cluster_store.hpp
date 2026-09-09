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
//
// Active-active additions (design 10 §10.3, plan 12 A2) — the failover files above are
// untouched, these live beside them:
//   epoch.<node>        that gateway's own epoch (its clientid/stateid/verifier source)
//   nodes/<node>        "<node_address>\n": where that gateway's fs_locations point
//   fence.<node>        "<expires_at_unix_ms> <fsid>:<epoch>[,<fsid>:<epoch>...]\n" — every
//                       fsid that node holds, one lease for all of them; an empty list is
//                       the node's heartbeat.  All fence.<node> writes go under fence.lock.
//   fs/<fsid>/epoch     that export's takeover generation, +1 per owner change
//   fs/<fsid>/owner     "<fs_epoch> <address> <node>\n": the current owner
//   fs/<fsid>/clients/<fnv64>   that export's reclaim list, kept by its owner
//
// Shared export catalog (design 11 §11.3, plan 12 A3), beside the above:
//   catalog.toml                the current catalog: whole-file atomic replace, the
//                               version inside its [catalog] header
//   catalog.history/<version>.toml   the content each commit replaced, newest 32 kept
//   catalog.lock                writer serialization (write_catalog's CAS)
//   catalog.<node>              "<applied_version> <digest> <applied_at_ms> <status>\n":
//                               what that gateway runs and how its last apply went
struct FenceRecord {
    std::string node;
    uint64_t epoch = 0;
    int64_t expires_at_ms = 0;  // wall clock (CLOCK_REALTIME); gateways must run NTP
};

// The current owner of one export, as fs/<fsid>/owner records it (plan 12 A2).
struct OwnerRecord {
    std::string node;
    std::string address;  // that node's `[cluster] node_address`
    uint64_t fs_epoch = 0;
};

// The catalog document as stored: its text verbatim plus the version peeked from the
// [catalog] header (plan 12 A3).
struct CatalogDoc {
    uint64_t version = 0;
    std::string text;
};

// One gateway's catalog.<node> record: the version it last applied (or last tried),
// the export digest it runs, when, and "ok" | "error:<text>".
struct CatalogApplied {
    std::string node;
    uint64_t version = 0;
    std::string digest;
    int64_t applied_at_ms = 0;
    std::string status;
};

// One node's batched fence record: the exports it holds under one lease.
struct NodeFences {
    struct Hold {
        uint32_t fsid = 0;
        uint64_t epoch = 0;  // the fs epoch the holder took the fence with
    };
    std::string node;
    int64_t expires_at_ms = 0;
    std::vector<Hold> holds;  // sorted by fsid
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
    virtual Result<FenceRecord> acquire_fence(std::string_view node, uint64_t epoch, std::chrono::milliseconds ttl,
                                              bool force) = 0;
    virtual Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) = 0;
    virtual Result<void> release_fence(std::string_view node) = 0;

    // Export-table digests: one record per node, overwritten on every start.
    virtual Result<void> put_exports_digest(std::string_view node, std::string_view digest) = 0;
    virtual Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() = 0;

    // ---- active-active (design 10 §10.3, plan 12 A2) ----------------------------------
    // Per-node: the gateway's own epoch (0 before the first bump; bumped once per process
    // start under active-active) and its advertised address.
    virtual Result<uint64_t> read_node_epoch(std::string_view node) = 0;
    virtual Result<uint64_t> bump_node_epoch(std::string_view node) = 0;
    virtual Result<void> put_node_address(std::string_view node, std::string_view address) = 0;
    virtual Result<std::vector<std::pair<std::string, std::string>>> list_nodes() = 0;

    // Per-fsid fences with a per-fsid interface over per-node records: read_fs_fence
    // answers the record naming `fsid` (a live one first; else the latest expired one, so
    // the caller sees who lapsed; nullopt when nobody names it).  acquire_fs_fence adds
    // `fsid` to `node`'s record (EBUSY while another node's live record names it, unless
    // `force`) and strips it from every other record so one export is never named by two.
    // renew_fences rewrites `node`'s whole record with a fresh expiry — one write however
    // many exports it holds; with no export it is a plain heartbeat.  release_fs_fence
    // drops `fsid` from `node`'s record (EPERM when another node holds it; ok when nobody
    // does).  list_fences returns every record, expired ones included.
    virtual Result<std::optional<FenceRecord>> read_fs_fence(uint32_t fsid) = 0;
    virtual Result<FenceRecord> acquire_fs_fence(uint32_t fsid, std::string_view node, uint64_t epoch,
                                                 std::chrono::milliseconds ttl, bool force) = 0;
    virtual Result<void> renew_fences(std::string_view node, std::chrono::milliseconds ttl) = 0;
    virtual Result<void> release_fs_fence(uint32_t fsid, std::string_view node) = 0;
    virtual Result<std::vector<NodeFences>> list_fences() = 0;

    // Per-fsid epoch, owner record and reclaim list.
    virtual Result<uint64_t> read_fs_epoch(uint32_t fsid) = 0;
    virtual Result<uint64_t> bump_fs_epoch(uint32_t fsid) = 0;
    virtual Result<std::optional<OwnerRecord>> read_owner(uint32_t fsid) = 0;
    virtual Result<void> put_owner(uint32_t fsid, const OwnerRecord& owner) = 0;
    virtual Result<std::vector<std::string>> list_clients(uint32_t fsid) = 0;
    virtual Result<void> put_client(uint32_t fsid, std::string_view owner_id) = 0;
    virtual Result<void> erase_client(uint32_t fsid, std::string_view owner_id) = 0;

    // ---- shared export catalog (design 11 §11.3, plan 12 A3) ---------------------------
    // read_catalog: nullopt before the first publish; EINVAL when the file carries no
    // parseable [catalog] version.  write_catalog is the CAS commit: `text` must carry
    // version expected + 1 in its header (EINVAL otherwise — the store never edits the
    // document), the stored version must equal `expected` (0 = none yet; EAGAIN
    // otherwise, the loser re-reads and retries), the replaced content is copied to
    // catalog.history/<version>.toml first and the history pruned to the newest 32.
    // Returns the new version.
    virtual Result<std::optional<CatalogDoc>> read_catalog() = 0;
    virtual Result<uint64_t> write_catalog(uint64_t expected, std::string_view text) = 0;
    // History: the versions kept, ascending (the current one is not among them);
    // read_catalog_history is ENOENT for a version not kept.
    virtual Result<std::vector<uint64_t>> list_catalog_history() = 0;
    virtual Result<CatalogDoc> read_catalog_history(uint64_t version) = 0;
    // catalog.<node>: one record per node, overwritten on every apply; listed by node.
    // The node names "toml", "lock" and "history" are reserved (EINVAL).
    virtual Result<void> put_catalog_applied(const CatalogApplied& applied) = 0;
    virtual Result<std::vector<CatalogApplied>> list_catalog_applied() = 0;
};

// How many replaced catalog versions write_catalog keeps under catalog.history/.
inline constexpr size_t kCatalogHistoryKeep = 32;

// Clock-skew allowance applied to fence expiry checks.
inline constexpr std::chrono::milliseconds kFenceSkewTolerance{500};

// The POSIX-directory implementation.  `stale_lock_after`: a `.lock` file older than
// this belongs to a dead writer and is reclaimed (2 × fence_lease in the controller).
// The directory (and clients/) is created on first use.
std::unique_ptr<ClusterStore> make_posix_cluster_store(
    std::string shared_dir, std::chrono::milliseconds stale_lock_after = std::chrono::seconds(6));

}  // namespace lnfs::server
