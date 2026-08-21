#pragma once
// v4.1 state manager, phase-3 scope (design 07 §7.1–7.3/7.5): client and session
// tables, SEQUENCE slot fast path with exactly-once replay cache, lock-free lease
// renewal, clients/ stable-list persistence and the grace skeleton — plus the minimal
// open-state table a read-only mount needs (Linux clients OPEN before every READ).
//
// Locking (07 §7.2 instantiated): every operation holds at most ONE shard AsyncMutex
// at a time and never performs backend IO under it, which trivially satisfies the
// ①session ②client ③objlock ④state ordering and is deadlock-free by construction —
// verified by the concurrency matrix test.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "backend/api.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "runtime/sync.hpp"
#include "runtime/task.hpp"

namespace lnfs::state {

using nfsv4::SessionId;
using nfsv4::Stateid;
using nfsv4::Verifier;

struct ClientRec {
  uint64_t clientid = 0;  // {boot_epoch(32) | counter(32)}
  std::string owner_id;   // EXCHANGE_ID co_ownerid (raw bytes)
  std::string principal;  // RPC identity that registered the owner (CLID_INUSE checks)
  bool confirmed = false;
  Verifier verifier{};
  uint32_t cs_sequence = 1;  // next expected CREATE_SESSION sequence
  std::vector<std::byte> cs_cached_reply;  // CREATE_SESSION replay (op body bytes)
  std::vector<SessionId> sessions;
  std::atomic<int64_t> lease_expiry{0};  // coarse seconds; SEQUENCE fast path stores
  bool reclaim_complete = false;
  bool persisted = false;
};

struct Slot {
  uint32_t seqid = 0;  // last executed sequence on this slot (0 = none yet)
  bool in_flight = false;
  bool cached = false;
  std::vector<std::byte> reply;  // full RPC reply payload for replay
};

struct SessionRec {
  SessionId id{};
  std::shared_ptr<ClientRec> client;
  nfsv4::ChannelAttrs fore, back;
  std::vector<Slot> slots;
  std::unordered_set<uint64_t> bound_conns;  // CREATE/BIND/SEQUENCE bind; DESTROY checks
};

struct OpenRec {
  uint64_t clientid = 0;
  uint32_t fsid = 0;
  backend::ObjId oid{};
  uint32_t seqid = 1;
};

class StateMgr {
 public:
  struct Config {
    uint64_t boot_epoch = 1;
    std::string state_dir;
    uint32_t lease_seconds = nfsv4::kLeaseSeconds;
    uint32_t max_slots = 32;
    uint32_t max_cached_reply = 8u << 10;
    uint32_t max_ops = 64;
    uint32_t max_io = 1u << 20;
  };

  explicit StateMgr(Config cfg);
  const Config& config() const { return cfg_; }

  // ---- grace (7.5 skeleton; read path never blocks in phase 3) ----
  void load_grace_list();  // reads state_dir/clients/, arms the grace deadline
  bool in_grace() const;
  bool in_stable_list(std::string_view owner_id) const;

  // ---- EXCHANGE_ID ----
  struct ExchangeResult {
    uint32_t status = 0;  // nfsstat4
    uint64_t clientid = 0;
    uint32_t sequenceid = 1;
    bool confirmed_r = false;  // reply flag EXCHGID4_FLAG_CONFIRMED_R
  };
  // Full RFC 8881 §18.35 record semantics: confirmed/unconfirmed per owner, principal
  // collisions, client-reboot detection, and the UPD_CONFIRMED_REC_A update path.
  rt::Task<ExchangeResult> exchange_id(std::string owner_id, Verifier verifier,
                                       std::string principal, bool update);

  // ---- CREATE_SESSION (replay-protected by clientid+sequence) ----
  struct CreateSessionResult {
    uint32_t status = 0;
    bool replay = false;
    std::vector<std::byte> cached;  // replay: previously encoded op body
    SessionId sessionid{};
    nfsv4::ChannelAttrs fore, back;
    uint32_t sequence = 0;
  };
  rt::Task<CreateSessionResult> create_session(uint64_t clientid, uint32_t sequence,
                                               std::string principal,
                                               const nfsv4::ChannelAttrs& fore_req,
                                               const nfsv4::ChannelAttrs& back_req,
                                               uint64_t conn_id);
  // Called after the engine encoded the successful reply op body: persists the client
  // to the stable list and caches the reply for sequence-based replay.
  rt::Task<void> confirm_create_session(uint64_t clientid, std::vector<std::byte> reply);

  rt::Task<uint32_t> destroy_session(const SessionId& id, uint64_t conn_id);
  rt::Task<uint32_t> destroy_clientid(uint64_t clientid);
  rt::Task<uint32_t> bind_conn(const SessionId& id, uint64_t conn_id);

  // ---- SEQUENCE ----
  struct SeqResult {
    uint32_t status = 0;          // 0: proceed (owner) or replay
    bool replay = false;
    std::vector<std::byte> replay_bytes;  // full RPC reply payload
    // negotiated context for the executing compound:
    uint32_t max_ops = 0, max_request = 0, max_response = 0, max_response_cached = 0;
    uint32_t highest_slot = 0;
  };
  rt::Task<SeqResult> sequence_begin(const SessionId& id, uint32_t slotid, uint32_t seqid,
                                     uint32_t highest, bool cachethis, uint64_t conn_id);
  // Releases a claimed slot without advancing its sequence (SEQUENCE-level failure).
  rt::Task<void> sequence_abort(const SessionId& id, uint32_t slotid);
  // Completes an owned slot: caches the reply when requested and within budget.
  rt::Task<void> sequence_complete(const SessionId& id, uint32_t slotid, uint32_t seqid,
                                   bool cache, std::vector<std::byte> reply);

  rt::Task<uint32_t> reclaim_complete(uint64_t clientid);

  // ---- minimal open-state (read-only IO) ----
  rt::Task<Stateid> open_read(uint64_t clientid, uint32_t fsid, const backend::ObjId& oid);
  struct StateLookup {
    uint32_t status = 0;  // OK / BAD_STATEID / STALE_STATEID
    bool special = false;
    OpenRec rec{};  // valid when status==0 && !special
  };
  rt::Task<StateLookup> lookup_stateid(const Stateid& sid);
  rt::Task<uint32_t> close_state(const Stateid& sid);
  rt::Task<uint32_t> free_stateid(const Stateid& sid);

  struct Stats {
    size_t clients = 0, sessions = 0, opens = 0;
    uint64_t seq_new = 0, seq_replay = 0, seq_misordered = 0, seq_waits = 0;
    bool grace = false;
  };
  Stats stats() const;

 private:
  static constexpr size_t kShards = 16;
  struct OwnerSlot {
    std::shared_ptr<ClientRec> confirmed;
    std::shared_ptr<ClientRec> unconfirmed;
  };
  struct ClientShard {
    rt::AsyncMutex mu;
    std::unordered_map<uint64_t, std::shared_ptr<ClientRec>> by_id;
    std::unordered_map<std::string, OwnerSlot> by_owner;
  };
  struct SessionIdHash {
    size_t operator()(const SessionId& id) const noexcept;
  };
  struct OtherHash {
    size_t operator()(const std::array<std::byte, 12>& other) const noexcept;
  };
  struct SessionShard {
    rt::AsyncMutex mu;
    rt::AsyncCondVar cv;  // in-flight slot duplicate waiters
    std::unordered_map<SessionId, std::shared_ptr<SessionRec>, SessionIdHash> table;
  };
  struct StateShard {
    rt::AsyncMutex mu;
    std::unordered_map<std::array<std::byte, 12>, OpenRec, OtherHash> table;
  };

  ClientShard& client_shard(uint64_t clientid);
  SessionShard& session_shard(const SessionId& id);
  StateShard& state_shard(const std::array<std::byte, 12>& other);
  int64_t now_coarse() const;
  void renew(ClientRec& client);
  void persist_client(const ClientRec& client);
  void unpersist_client(const ClientRec& client);
  void note_reclaimed(std::string_view owner_id);  // grace early-exit bookkeeping

  Config cfg_;
  std::array<ClientShard, kShards> clients_;
  std::array<SessionShard, kShards> sessions_;
  std::array<StateShard, kShards> states_;
  std::atomic<uint32_t> next_client_{1};
  std::atomic<uint64_t> next_state_{1};
  std::atomic<uint32_t> next_session_{1};

  mutable std::mutex grace_mu_;
  std::unordered_set<std::string> grace_pending_;  // owner ids still expected to reclaim
  std::unordered_set<std::string> stable_list_;
  std::chrono::steady_clock::time_point grace_deadline_{};
  mutable std::atomic<bool> grace_active_{false};

  mutable std::atomic<uint64_t> seq_new_{0}, seq_replay_{0}, seq_misordered_{0},
      seq_waits_{0};
  std::atomic<int64_t> client_count_{0}, session_count_{0}, open_count_{0};
};

}  // namespace lnfs::state
