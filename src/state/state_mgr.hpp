#pragma once
// v4.1 state manager (design 07 §7.1–7.5, phase-4 scope): client and session tables,
// SEQUENCE slot fast path with exactly-once replay cache, lock-free lease renewal,
// clients/ stable-list persistence, the full open-state table (StateTable +
// FileStateIdx: share-reservation arbitration, same-owner merge, OPEN_DOWNGRADE,
// stateid seqid discipline), the lease scanner with courtesy state and the reclaim
// chain, and a complete grace/reclaim gate.
//
// Locking (07 §7.2 instantiated): every operation holds at most ONE shard AsyncMutex
// at a time and never performs backend IO under it, which trivially satisfies the
// ①session ②client ③objlock ④state ordering and is deadlock-free by construction —
// verified by the concurrency matrix test.  Backend OpenPtr release (the only backend
// side effect a state op owns) always happens after the last lock is dropped.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "backend/api.hpp"
#include "nfsv4/callback.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "runtime/sync.hpp"
#include "runtime/task.hpp"
#include "state/lock_mgr.hpp"
#include "transport/backchannel.hpp"

namespace lnfs::state {

using nfsv4::SessionId;
using nfsv4::Stateid;
using nfsv4::Verifier;
using StateOther = std::array<std::byte, 12>;

struct OtherHash {
  size_t operator()(const StateOther& other) const noexcept;
};

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
  // Lease scanner (07 §7.4): a client past its lease enters courtesy; a courtesy client
  // is reclaimed on the first conflict or after courtesy_multiplier × lease.
  std::atomic<bool> courtesy{false};
  std::atomic<bool> expired{false};  // reclaim chain started: its state is dead
  int64_t courtesy_since = 0;
  // Deadline of this client's live lease-heap entry, -1 = none. Guarded by the
  // StateMgr lease heap mutex (plan doc 10 §2.6: the scanner walks an expiry heap
  // instead of every client every second; renewals still never touch it).
  int64_t lease_heap_deadline = -1;
  // Read delegations held (plan doc 10 §5.2); drives SEQ4_STATUS_CB_PATH_DOWN.
  std::atomic<uint32_t> delegs{0};
  std::unordered_set<StateOther, OtherHash> states;  // owned stateids (client shard)
};

struct Slot {
  uint32_t seqid = 0;  // last executed sequence on this slot (0 = none yet)
  bool in_flight = false;
  bool cached = false;
  std::vector<std::byte> reply;  // full RPC reply payload for replay
  // Retransmission-of-in-flight waiters park here (plan doc 10 §2.6): completing one
  // slot no longer wakes every waiter of the whole session shard.
  rt::AsyncCondVar cv;
};

struct SessionRec {
  SessionId id{};
  std::shared_ptr<ClientRec> client;
  nfsv4::ChannelAttrs fore, back;
  // Fixed at creation; Slot owns a condvar and never moves.
  std::unique_ptr<Slot[]> slots;
  uint32_t slot_count = 0;
  std::unordered_set<uint64_t> bound_conns;  // CREATE/BIND/SEQUENCE bind; DESTROY checks

  // Backchannel send side (plan doc 10 §5.2): one cb slot (the back channel is
  // clamped to 1 request), bound at CREATE_SESSION (CONN_BACK_CHAN flag) or
  // BIND_CONN_TO_SESSION(BACK/BOTH).  cb_mu guards all of it and is never held
  // across IO — a call marks the slot busy, sends, then clears it; a slot stuck
  // longer than a lease means the client stopped answering and the path is down.
  std::mutex cb_mu;
  std::shared_ptr<transport::CbChannel> cb_chan;
  uint32_t cb_program = 0;
  nfsv4::cb::Cred cb_cred;
  uint32_t cb_seq = 0;
  bool cb_inflight = false;
  int64_t cb_inflight_since = 0;
  std::atomic<bool> cb_down{false};
};

enum class StateType : uint8_t { kOpen = 1, kLock = 2, kDeleg = 3 };

// share_access / share_deny bit values (RFC 8881 §18.16).
inline constexpr uint32_t kShareRead = 1, kShareWrite = 2, kShareBoth = 3;

struct StateRec {  // other = {boot_epoch(4B)|type(1B)|counter(7B)}
  StateOther other{};
  StateType type = StateType::kOpen;
  // seqid/access/deny are mutated under the file shard (merge / downgrade) and read
  // on the IO path without it: relaxed atomics keep those reads well-defined.
  std::atomic<uint32_t> seqid{1};  // stateid version: bumped by OPEN merge / DOWNGRADE
  std::shared_ptr<ClientRec> client;
  uint32_t fsid = 0;
  backend::ObjId oid{};
  // kOpen:
  std::string owner;  // open_owner4.owner / lock_owner4.owner bytes
  std::atomic<uint32_t> access{0}, deny{0};
  // kLock:
  backend::LockOwnerId lowner{};  // {clientid(8) | fnv64(owner)(8) | len(4)}
  std::shared_ptr<StateRec> parent_open;  // the open stateid the lock derives from
  // Backend open handle: written only under the state shard lock (creation before
  // publication, taken out by unlink) and copied under it for the IO path; the
  // handle itself is released outside every lock.
  backend::OpenPtr bopen;
  bool closed = false;  // set under the state shard when removed from the index
  // kDeleg (plan doc 10 §5.2): the filehandle for CB_RECALL, the recall state and
  // the revocation deadline (coarse seconds; 0 = not recalled).
  std::vector<std::byte> fh;
  std::atomic<bool> recalled{false};
  std::atomic<int64_t> recall_deadline{0};
};
using StateRef = std::shared_ptr<StateRec>;

struct FileStateRec {  // conflict arbitration entry: share reservations + lock owners
  std::vector<StateRef> opens;
  std::vector<StateRef> locks;  // one lock stateid per (lock-owner, file)
  std::vector<StateRef> delegs;  // read delegations (plan doc 10 §5.2)
};

// Compatibility view for callers that only need the identity of an open state.
struct OpenRec {
  uint64_t clientid = 0;
  uint32_t fsid = 0;
  backend::ObjId oid{};
  uint32_t seqid = 1;
  uint32_t access = 0, deny = 0;
};

class StateMgr {
 public:
  struct Config {
    uint64_t boot_epoch = 1;
    std::string state_dir;
    uint32_t lease_seconds = nfsv4::kLeaseSeconds;
    // Grace window after restart (plan doc 10 §4.4): 0 = lease_seconds. Decoupled so
    // operators can shorten recovery (grace < lease) without touching the lease.
    uint32_t grace_seconds = 0;
    uint32_t courtesy_multiplier = 24;  // courtesy window = multiplier × lease
    uint32_t max_slots = 32;
    uint32_t max_cached_reply = 8u << 10;
    uint32_t max_ops = 64;
    uint32_t max_io = 1u << 20;
    // Resource caps (plan doc 10 §1.5): a runaway client gets NFS4ERR_RESOURCE
    // instead of growing server state without bound.  0 = unlimited.
    uint32_t max_clients = 4096;
    uint32_t max_states_per_client = 65536;      // open + lock stateids per client
    uint32_t max_lock_segments_per_owner = 1024; // per (lock-owner, file)
    // Shard count for the client/session/state/file tables (plan doc 10 §2.6: was a
    // hardcoded 16).  [server] state_shards.
    uint32_t shards = 16;
    // Read delegations (plan doc 10 §5.2); [protocol] delegations kill switch.
    bool delegations = true;
  };

  explicit StateMgr(Config cfg);
  const Config& config() const { return cfg_; }

  // ---- grace (7.5) ----
  void load_grace_list();  // reads state_dir/clients/, arms the grace deadline
  // Operator override (`lightnfs-ctl grace-end`, plan doc 10 §4.2): ends the grace
  // period immediately.  Clients that had not reclaimed yet lose their claim window.
  // Returns whether grace was active.
  bool end_grace();
  bool in_grace() const;
  bool in_stable_list(std::string_view owner_id) const;
  int64_t grace_remaining_seconds() const;

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
  // cb_chan non-null = the client set CONN_BACK_CHAN: this connection carries the
  // backchannel.  cb_program/cb_cred are remembered either way (a later
  // BIND_CONN_TO_SESSION may attach a connection).
  rt::Task<CreateSessionResult> create_session(
      uint64_t clientid, uint32_t sequence, std::string principal,
      const nfsv4::ChannelAttrs& fore_req, const nfsv4::ChannelAttrs& back_req,
      uint64_t conn_id, std::shared_ptr<transport::CbChannel> cb_chan = nullptr,
      uint32_t cb_program = 0, nfsv4::cb::Cred cb_cred = {});
  // Called after the engine encoded the successful reply op body: persists the client
  // to the stable list and caches the reply for sequence-based replay.
  rt::Task<void> confirm_create_session(uint64_t clientid, std::vector<std::byte> reply);

  rt::Task<uint32_t> destroy_session(const SessionId& id, uint64_t conn_id);
  rt::Task<uint32_t> destroy_clientid(uint64_t clientid);
  rt::Task<uint32_t> bind_conn(const SessionId& id, uint64_t conn_id);
  // BIND_CONN_TO_SESSION with a BACK direction: attach this connection's channel.
  rt::Task<uint32_t> bind_backchannel(const SessionId& id,
                                      std::shared_ptr<transport::CbChannel> chan);

  // ---- SEQUENCE ----
  struct SeqResult {
    uint32_t status = 0;          // 0: proceed (owner) or replay
    bool replay = false;
    std::vector<std::byte> replay_bytes;  // full RPC reply payload
    // negotiated context for the executing compound:
    uint32_t max_ops = 0, max_request = 0, max_response = 0, max_response_cached = 0;
    uint32_t highest_slot = 0;
    uint32_t status_flags = 0;  // sr_status_flags: SEQ4_STATUS_CB_PATH_DOWN etc.
  };
  rt::Task<SeqResult> sequence_begin(const SessionId& id, uint32_t slotid, uint32_t seqid,
                                     uint32_t highest, bool cachethis, uint64_t conn_id);
  // Releases a claimed slot without advancing its sequence (SEQUENCE-level failure).
  rt::Task<void> sequence_abort(const SessionId& id, uint32_t slotid);
  // Completes an owned slot: caches the reply when requested and within budget.
  rt::Task<void> sequence_complete(const SessionId& id, uint32_t slotid, uint32_t seqid,
                                   bool cache, std::vector<std::byte> reply);

  rt::Task<uint32_t> reclaim_complete(uint64_t clientid);

  // ---- open state (7.1 StateTable + FileStateIdx) ----
  struct OpenArgs {
    uint64_t clientid = 0;
    uint32_t fsid = 0;
    backend::ObjId oid{};
    std::string owner;
    uint32_t access = 0, deny = 0;
    bool reclaim = false;  // CLAIM_PREVIOUS: stable-list gated, allowed during grace
    // CLAIM_DELEG_CUR_FH (plan doc 10 §5.2): the open reclaims state under a
    // delegation being recalled — it must not itself trigger the recall gate.
    bool deleg_claim = false;
  };
  struct OpenResult {
    uint32_t status = 0;  // OK / SHARE_DENIED / GRACE / NO_GRACE / RECLAIM_BAD / ...
    Stateid stateid{};
    bool merged = false;  // existing same-owner state widened instead of a new record
  };
  // Share-reservation arbitration against every other open on the file (courtesy
  // conflicts are reclaimed first, then re-arbitrated); same owner+file merges into the
  // existing record (access/deny union, seqid++).  `bopen` is the backend handle the
  // engine already obtained (may be null for backends without open state).
  rt::Task<OpenResult> open(OpenArgs args, backend::OpenPtr bopen);
  rt::Task<Stateid> open_read(uint64_t clientid, uint32_t fsid, const backend::ObjId& oid);

  // ---- read delegations (plan doc 10 §5.2) ----
  struct DelegGrant {
    bool granted = false;
    Stateid stateid{};
  };
  // Grant policy: delegations enabled, not in grace, no reclaim, the session has a
  // live backchannel, the open is read-only, no other client holds a write open, and
  // this client holds no delegation on the file yet.  `fh` is kept for CB_RECALL.
  rt::Task<DelegGrant> maybe_grant_read_deleg(const SessionId& sessionid, uint64_t clientid,
                                              uint32_t fsid, const backend::ObjId& oid,
                                              std::vector<std::byte> fh, uint32_t access,
                                              bool reclaim);
  rt::Task<uint32_t> delegreturn(const Stateid& sid, uint64_t clientid, uint32_t fsid,
                                 const backend::ObjId& oid);
  // Mutation gate: when the file carries read delegations, initiate CB_RECALLs and
  // return NFS4ERR_DELAY (the client retries; DELEGRETURN or the revocation deadline
  // clears the way).  Returns 0 when the file is delegation-free.
  rt::Task<uint32_t> deleg_conflict(uint32_t fsid, const backend::ObjId& oid);
  // CLAIM_DELEG_CUR_FH: the stateid must be this client's delegation on this file.
  rt::Task<uint32_t> check_deleg_claim(const Stateid& sid, uint64_t clientid,
                                       uint32_t fsid, const backend::ObjId& oid);

  // ---- CB_NOTIFY_LOCK (plan doc 10 §5.2) ----
  // Registers a blocked LOCK waiter; when a lock on the file is released the client
  // gets CB_NOTIFY_LOCK and retries.  Best effort, capped and lease-bounded.
  void register_lock_waiter(uint32_t fsid, const backend::ObjId& oid, uint64_t clientid,
                            std::string owner, std::vector<std::byte> fh);

  // IO-path stateid check (07 §6.1 order: special → table → OPENMODE).  `need` is the
  // share_access bit the op requires (kShareRead / kShareWrite).
  struct IoCheck {
    uint32_t status = 0;  // OK / BAD_STATEID / STALE_STATEID / OLD_STATEID / OPENMODE /
                          // LOCKED (anonymous stateid vs. deny) / GRACE
    bool special = false;
    StateRef rec;
    backend::OpenPtr bopen;  // the open's backend handle (may be null); keeps it alive
  };
  rt::Task<IoCheck> check_io(const Stateid& sid, uint64_t clientid, uint32_t fsid,
                             const backend::ObjId& oid, uint32_t need);

  struct StateLookup {
    uint32_t status = 0;  // OK / BAD_STATEID / STALE_STATEID
    bool special = false;
    OpenRec rec{};  // valid when status==0 && !special
  };
  rt::Task<StateLookup> lookup_stateid(const Stateid& sid);

  // CLOSE: seqid discipline (0 = current, older → OLD_STATEID, ahead → BAD_STATEID).
  // Returns the post-close stateid in `out`.
  rt::Task<uint32_t> close_state(const Stateid& sid, uint64_t clientid, Stateid* out);
  rt::Task<uint32_t> close_state(const Stateid& sid);  // legacy: any owner, any seqid
  rt::Task<uint32_t> open_downgrade(const Stateid& sid, uint64_t clientid, uint32_t access,
                                    uint32_t deny, Stateid* out);
  rt::Task<uint32_t> free_stateid(const Stateid& sid);

  // ---- byte-range locks (7.6; LOCK/LOCKT/LOCKU, RFC 8881 §18.10–§18.12) ----
  struct LockDenied {
    uint64_t offset = 0, length = 0;
    bool exclusive = false;
    uint64_t clientid = 0;
    std::string owner;  // lock_owner4.owner of the conflicting holder
  };
  struct LockArgs {
    uint64_t clientid = 0;
    uint32_t fsid = 0;
    backend::ObjId oid{};
    bool exclusive = false;
    bool reclaim = false;
    uint64_t offset = 0, length = 0;  // length UINT64_MAX = to EOF (validated by caller)
    bool new_owner = false;
    Stateid open_stateid{};  // new_owner: the open the lock derives from
    std::string owner;       // new_owner: lock_owner4.owner
    Stateid lock_stateid{};  // !new_owner: existing lock stateid
  };
  struct LockResult {
    uint32_t status = 0;  // OK / DENIED / BAD_STATEID / OLD_STATEID / OPENMODE / GRACE ...
    Stateid stateid{};    // OK: the (new or bumped) lock stateid
    LockDenied denied{};  // status == DENIED
  };
  rt::Task<LockResult> lock(LockArgs args);
  rt::Task<LockResult> lockt(uint64_t clientid, uint32_t fsid, const backend::ObjId& oid,
                             std::string owner, bool exclusive, uint64_t offset,
                             uint64_t length);
  rt::Task<uint32_t> locku(const Stateid& sid, uint64_t clientid, uint64_t offset,
                           uint64_t length, Stateid* out);
  GatewayLockMgr& lock_table() { return locks_; }
  // One scanner pass: expired leases → courtesy; courtesy beyond the window → reclaim.
  rt::Task<void> scan_leases();
  // Runs scan_leases once per second until *stop becomes true.
  rt::Task<void> run_lease_scanner(std::atomic<bool>* stop);
  // Reclaim chain for one client (forced via lightnfs-ctl, courtesy conflict, or
  // timeout): StateRec → OpenPtr release → FileStateIdx unlink → ClientRec → sessions
  // → stable list.  Returns STALE_CLIENTID if unknown.
  rt::Task<uint32_t> expire_client(uint64_t clientid);

  struct Stats {
    size_t clients = 0, sessions = 0, opens = 0, files = 0, courtesy = 0;
    uint64_t seq_new = 0, seq_replay = 0, seq_misordered = 0, seq_waits = 0;
    uint64_t lease_expirations = 0;  // clients that entered courtesy
    uint64_t reclaim_conflict = 0, reclaim_timeout = 0, reclaim_forced = 0;
    uint64_t share_denied = 0, open_merges = 0;
    size_t lock_states = 0, lock_segments = 0, lock_owners = 0;
    uint64_t lock_denied = 0;
    // Delegations + backchannel (plan doc 10 §5.2).
    size_t delegs = 0;
    uint64_t deleg_grants = 0, deleg_recalls = 0, deleg_returns = 0, deleg_revokes = 0;
    uint64_t cb_lock_notifies = 0;
    bool grace = false;
    int64_t grace_remaining = 0;
  };
  Stats stats() const;
  // Human-readable table dump for lightnfs-ctl (clients, sessions, open states).
  rt::Task<std::string> dump();

 private:
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
  struct SessionShard {
    rt::AsyncMutex mu;
    std::unordered_map<SessionId, std::shared_ptr<SessionRec>, SessionIdHash> table;
  };
  struct StateShard {
    rt::AsyncMutex mu;
    std::unordered_map<StateOther, StateRef, OtherHash> table;
  };
  struct FileShard {
    rt::AsyncMutex mu;
    std::unordered_map<FileKey, FileStateRec, FileKeyHash> table;
  };

  ClientShard& owner_shard(std::string_view owner_id);  // clients live in the owner shard
  SessionShard& session_shard(const SessionId& id);
  StateShard& state_shard(const StateOther& other);
  FileShard& file_shard(const FileKey& key);
  int64_t now_coarse() const;
  void renew(ClientRec& client);
  void persist_client(const ClientRec& client);
  void unpersist_client(const ClientRec& client);
  void note_reclaimed(std::string_view owner_id);  // grace early-exit bookkeeping
  // O(1) clientid lookup via the dedicated index (plan doc 10 §2.6: replaces the
  // all-shards sequential locked scan). Sync; Task kept for call-site compatibility.
  rt::Task<std::shared_ptr<ClientRec>> find_client(uint64_t clientid);
  std::shared_ptr<ClientRec> find_client_sync(uint64_t clientid);
  // by_id companions: every by_id insert/erase mirrors into the clientid index.
  void index_client(const std::shared_ptr<ClientRec>& rec);
  void unindex_client(uint64_t clientid);
  // Arms (or advances) the client's lease-heap entry; earliest deadline wins.
  void arm_lease_check(ClientRec& client, int64_t when);
  // Wakes every slot's retransmission waiters; used when a session is torn down.
  static void wake_all_slots(SessionRec& session);
  StateOther new_other(StateType type);
  // Internal reclaim chain; `reason` selects the metric.
  rt::Task<uint32_t> expire_client_impl(uint64_t clientid, int reason);
  // Drops one state from the stateid table, the file index and its client.  An open
  // state takes its lock states (and their ranges) with it.
  rt::Task<backend::OpenPtr> unlink_state(const StateRef& rec, bool from_client);
  static backend::LockOwnerId make_lowner(uint64_t clientid, std::string_view owner);
  // Resolves a lock-owner id to its holder (conflict reporting / courtesy reclaim).
  struct LockOwnerRec {
    std::shared_ptr<ClientRec> client;
    std::string owner;
  };
  std::optional<LockOwnerRec> find_lock_owner(const backend::LockOwnerId& id);
  void remember_lock_owner(const backend::LockOwnerId& id, std::shared_ptr<ClientRec> client,
                           std::string owner);

  Config cfg_;
  GatewayLockMgr locks_;
  std::mutex lock_owner_mu_;
  std::unordered_map<std::string, LockOwnerRec> lock_owners_;  // key: 24 id bytes
  size_t shard_count_;  // cfg_.shards, clamped >= 1
  std::unique_ptr<ClientShard[]> clients_;
  std::unique_ptr<SessionShard[]> sessions_;
  std::unique_ptr<StateShard[]> states_;
  std::unique_ptr<FileShard[]> files_;
  // clientid -> record, maintained alongside every by_id mutation (plan doc 10 §2.6).
  mutable std::mutex client_idx_mu_;
  std::unordered_map<uint64_t, std::shared_ptr<ClientRec>> client_idx_;
  // Lease expiry heap (plan doc 10 §2.6): (deadline, clientid), lazily re-armed; the
  // SEQUENCE renewal fast path stays a lone atomic store.
  std::mutex lease_heap_mu_;
  std::priority_queue<std::pair<int64_t, uint64_t>,
                      std::vector<std::pair<int64_t, uint64_t>>, std::greater<>>
      lease_heap_;
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
  std::atomic<int64_t> client_count_{0}, session_count_{0}, open_count_{0},
      file_count_{0}, courtesy_count_{0};
  std::atomic<uint64_t> lease_expirations_{0}, reclaim_conflict_{0},
      reclaim_timeout_{0}, reclaim_forced_{0}, share_denied_{0}, open_merges_{0},
      lock_denied_{0};
  std::atomic<int64_t> lock_count_{0};

  // ---- read delegations + backchannel (plan doc 10 §5.2) ----
  // Starts a recall for one delegation (marks it, arms the revocation deadline,
  // spawns the CB_RECALL sender on the current reactor).  Never called under a lock.
  void start_recall(const StateRef& deleg);
  rt::Task<void> recall_task(StateRef deleg);
  // One serialized CB_COMPOUND on the client's first live backchannel session;
  // build(target) produces the record.  False = no usable backchannel / send failed.
  rt::Task<bool> send_cb(std::shared_ptr<ClientRec> client,
                         std::vector<std::byte> (*build)(const nfsv4::cb::Target&,
                                                         const void*),
                         const void* args);
  // Sweeps recalled delegations whose deadline passed (called by scan_leases).
  rt::Task<void> sweep_recalls();
  void notify_lock_waiters(const FileKey& key);
  rt::Task<void> notify_task(uint64_t clientid, std::string owner,
                             std::vector<std::byte> fh);

  std::mutex recall_mu_;
  std::vector<StateRef> recall_watch_;  // recalled delegations awaiting return/revoke

  struct LockWaiter {
    uint64_t clientid = 0;
    std::string owner;
    std::vector<std::byte> fh;
    int64_t expires = 0;
  };
  std::mutex lock_waiters_mu_;
  std::unordered_map<FileKey, std::vector<LockWaiter>, FileKeyHash> lock_waiters_;

  std::atomic<int64_t> deleg_count_{0};
  std::atomic<uint64_t> deleg_grants_{0}, deleg_recalls_{0}, deleg_returns_{0},
      deleg_revokes_{0}, cb_lock_notifies_{0};
};

}  // namespace lnfs::state
