#pragma once
// Duplicate request cache (design 03 §3.7, nfsv3 research 9.2).  v3-only: the
// non-idempotent procedures (SETATTR/CREATE/MKDIR/SYMLINK/MKNOD/REMOVE/RMDIR/RENAME/
// LINK) register here before executing; retransmissions replay the cached reply instead
// of re-executing.  v4.1 session slots replace this and never enter.
//
// Three states per key: miss (caller executes and completes), in-progress (retransmit
// waits on the original — never concurrent re-execution), done (replay cached bytes).
// Entries expire after a TTL and are evicted oldest-first when total cached bytes
// exceed the memory cap.  Keys include a checksum of the argument prefix so a colliding
// xid from the same peer cannot replay a different call's reply.

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "runtime/sync.hpp"
#include "runtime/task.hpp"

namespace lnfs::rpc {

class Drc {
 public:
  struct Config {
    std::chrono::milliseconds ttl{120000};
    size_t max_memory = 64u << 20;
  };

  struct Key {
    // Binary peer identity: 16-byte address (v4 addresses v6-mapped) + port. Replaces
    // the per-request inet_ntop + string hash/compare (plan doc 10 §2.4).
    std::array<uint8_t, 16> peer_addr{};
    uint16_t peer_port = 0;
    uint32_t xid = 0;
    uint32_t prog = 0, vers = 0, proc = 0;
    uint64_t args_hash = 0;
    friend bool operator==(const Key&, const Key&) = default;

    static Key make(const sockaddr_storage& peer, uint32_t xid, uint32_t prog,
                    uint32_t vers, uint32_t proc, uint64_t args_hash);
  };

  struct Claim {
    bool owner = false;  // true: execute, then complete() or abort()
    // owner==false: the reply to retransmit — shared with the cache entry, no
    // under-lock value copy (plan doc 10 §2.4).
    std::shared_ptr<const std::vector<std::byte>> cached{};
  };

  struct Stats {
    uint64_t inserts = 0, replays = 0, waits = 0, evictions = 0;
    size_t entries = 0, bytes = 0;
  };

  explicit Drc(Config cfg) : cfg_(cfg) {}

  rt::Task<Claim> begin(const Key& key);
  rt::Task<void> complete(const Key& key, std::vector<std::byte> reply);
  rt::Task<void> abort(const Key& key);
  // Operator flush (`lightnfs-ctl drc flush`, plan doc 10 §4.2): drops every entry.
  // An in-progress owner completes into nothing; woken waiters re-execute — safe,
  // the DRC is a best-effort retransmission shield.  Returns entries dropped.
  rt::Task<size_t> flush();
  Stats stats() const;

 private:
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept;
  };
  struct Entry {
    bool done = false;
    std::shared_ptr<const std::vector<std::byte>> reply;
    std::chrono::steady_clock::time_point done_at{};
  };
  struct Shard {
    rt::AsyncMutex mu;
    rt::AsyncCondVar cv;
    std::unordered_map<Key, Entry, KeyHash> entries;
    // Completion order: front = oldest (TTL + memory evict). A deque, not a list:
    // no per-entry node allocation (plan doc 10 §2.6).
    std::deque<Key> completed;
    // Written under mu only; atomic so stats() can read without taking every shard
    // lock (plan doc 10 §3.8 — the plain reads were a TSAN-level data race).
    std::atomic<size_t> bytes{0};
    std::atomic<size_t> count{0};  // == entries.size()
  };

  Shard& shard_of(const Key& key) { return shards_[KeyHash{}(key) % kShards]; }
  void purge(Shard& sh);  // caller holds sh.mu

  static constexpr size_t kShards = 16;
  Config cfg_;
  std::array<Shard, kShards> shards_;
  mutable std::atomic<uint64_t> inserts_{0}, replays_{0}, waits_{0}, evictions_{0};
};

}  // namespace lnfs::rpc
