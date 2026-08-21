#include "state/state_mgr.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "util/log.hpp"

namespace lnfs::state {

using nfsv4::Status;

namespace {

uint64_t fnv64(std::string_view bytes) {
  uint64_t h = 1469598103934665603ull;
  for (char c : bytes) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

uint32_t as_u32(Status s) { return static_cast<uint32_t>(s); }

}  // namespace

size_t StateMgr::SessionIdHash::operator()(const SessionId& id) const noexcept {
  uint64_t h;
  std::memcpy(&h, id.data(), 8);
  uint64_t l;
  std::memcpy(&l, id.data() + 8, 8);
  return static_cast<size_t>(h ^ (l * 1099511628211ull));
}

size_t StateMgr::OtherHash::operator()(
    const std::array<std::byte, 12>& other) const noexcept {
  uint64_t h;
  std::memcpy(&h, other.data(), 8);
  uint32_t l;
  std::memcpy(&l, other.data() + 8, 4);
  return static_cast<size_t>(h ^ (uint64_t(l) << 17));
}

StateMgr::StateMgr(Config cfg) : cfg_(std::move(cfg)) {}

StateMgr::ClientShard& StateMgr::client_shard(uint64_t clientid) {
  return clients_[clientid % kShards];
}
StateMgr::SessionShard& StateMgr::session_shard(const SessionId& id) {
  return sessions_[SessionIdHash{}(id) % kShards];
}
StateMgr::StateShard& StateMgr::state_shard(const std::array<std::byte, 12>& other) {
  return states_[OtherHash{}(other) % kShards];
}

int64_t StateMgr::now_coarse() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void StateMgr::renew(ClientRec& client) {
  // 07 §7.2: the SEQUENCE fast path only does an atomic store; expiry scanning (phase 4)
  // reads it lazily.
  client.lease_expiry.store(now_coarse() + cfg_.lease_seconds, std::memory_order_relaxed);
}

// ---- grace -----------------------------------------------------------------

void StateMgr::load_grace_list() {
  std::lock_guard lock(grace_mu_);
  std::error_code ec;
  std::filesystem::create_directories(cfg_.state_dir + "/clients", ec);
  for (const auto& entry :
       std::filesystem::directory_iterator(cfg_.state_dir + "/clients", ec)) {
    std::string owner;
    if (FILE* f = fopen(entry.path().c_str(), "rb")) {
      char buf[nfsv4::kMaxOwnerId];
      size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      owner.assign(buf, n);
    }
    if (!owner.empty()) {
      stable_list_.insert(owner);
      grace_pending_.insert(owner);
    }
  }
  if (!grace_pending_.empty()) {
    grace_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.lease_seconds);
    grace_active_.store(true, std::memory_order_relaxed);
    LNFS_INFO("grace period armed: {} clients expected to reclaim, {}s window",
              grace_pending_.size(), cfg_.lease_seconds);
  }
}

bool StateMgr::in_grace() const {
  if (!grace_active_.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(grace_mu_);
  if (grace_pending_.empty() || std::chrono::steady_clock::now() >= grace_deadline_) {
    grace_active_.store(false, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool StateMgr::in_stable_list(std::string_view owner_id) const {
  std::lock_guard lock(grace_mu_);
  return stable_list_.contains(std::string(owner_id));
}

void StateMgr::note_reclaimed(std::string_view owner_id) {
  std::lock_guard lock(grace_mu_);
  grace_pending_.erase(std::string(owner_id));
  if (grace_pending_.empty() && grace_active_.load(std::memory_order_relaxed)) {
    grace_active_.store(false, std::memory_order_relaxed);
    LNFS_INFO("all listed clients reclaimed: leaving grace early");
  }
}

// ---- persistence -----------------------------------------------------------

void StateMgr::persist_client(const ClientRec& client) {
  std::string dir = cfg_.state_dir + "/clients";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char name[24];
  std::snprintf(name, sizeof name, "%016llx",
                static_cast<unsigned long long>(fnv64(client.owner_id)));
  std::string path = dir + "/" + name;
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    LNFS_WARN("cannot persist client record {}: reclaim after restart unavailable", path);
    return;
  }
  if (::write(fd, client.owner_id.data(), client.owner_id.size()) !=
      static_cast<ssize_t>(client.owner_id.size()))
    LNFS_WARN("short write persisting client record {}", path);
  (void)::fsync(fd);
  ::close(fd);
}

void StateMgr::unpersist_client(const ClientRec& client) {
  char name[24];
  std::snprintf(name, sizeof name, "%016llx",
                static_cast<unsigned long long>(fnv64(client.owner_id)));
  (void)::unlink((cfg_.state_dir + "/clients/" + name).c_str());
}

// ---- EXCHANGE_ID -----------------------------------------------------------

rt::Task<StateMgr::ExchangeResult> StateMgr::exchange_id(std::string owner_id,
                                                         Verifier verifier,
                                                         std::string principal,
                                                         bool update) {
  ExchangeResult out;
  ClientShard& shard = clients_[fnv64(owner_id) % kShards];
  auto lock = co_await shard.mu.lock();
  OwnerSlot& slot = shard.by_owner[owner_id];

  if (update) {  // EXCHGID4_FLAG_UPD_CONFIRMED_REC_A (RFC 8881 §18.35 update cases)
    if (!slot.confirmed) {
      out.status = as_u32(Status::kNoent);
    } else if (!(slot.confirmed->verifier == verifier)) {
      out.status = as_u32(Status::kNotSame);
    } else if (slot.confirmed->principal != principal) {
      out.status = as_u32(Status::kPerm);
    } else {
      renew(*slot.confirmed);
      out.clientid = slot.confirmed->clientid;
      out.sequenceid = slot.confirmed->cs_sequence;
      out.confirmed_r = true;
    }
    if (slot.confirmed == nullptr && slot.unconfirmed == nullptr)
      shard.by_owner.erase(owner_id);
    co_return out;
  }

  if (slot.confirmed) {
    ClientRec& c = *slot.confirmed;
    if (c.principal != principal) {
      if (!c.sessions.empty()) {  // held state: the owner string is taken
        out.status = as_u32(Status::kClidInuse);
        co_return out;
      }
      // RFC 8881 §18.35.4 case 3: no state under the old principal — remove the old
      // record and register the new principal's incarnation from scratch.
      unpersist_client(c);
      shard.by_id.erase(c.clientid);
      client_count_.fetch_sub(1, std::memory_order_relaxed);
      slot.confirmed.reset();
    } else if (c.verifier == verifier) {  // same incarnation: report confirmed record
      renew(c);
      out.clientid = c.clientid;
      out.sequenceid = c.cs_sequence;
      out.confirmed_r = true;
      co_return out;
    }
    // else: client rebooted — register a new unconfirmed record; the confirmed one
    // (and its state) survives until CREATE_SESSION confirms the new incarnation.
  }
  if (slot.unconfirmed) {  // always replaced by a fresh registration
    shard.by_id.erase(slot.unconfirmed->clientid);
    client_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  auto rec = std::make_shared<ClientRec>();
  rec->clientid = (cfg_.boot_epoch << 32) |
                  next_client_.fetch_add(1, std::memory_order_relaxed);
  rec->owner_id = owner_id;
  rec->principal = std::move(principal);
  rec->verifier = verifier;
  renew(*rec);
  slot.unconfirmed = rec;
  shard.by_id[rec->clientid] = rec;
  client_count_.fetch_add(1, std::memory_order_relaxed);
  out.clientid = rec->clientid;
  out.sequenceid = rec->cs_sequence;
  co_return out;
}

// ---- CREATE_SESSION --------------------------------------------------------

rt::Task<StateMgr::CreateSessionResult> StateMgr::create_session(
    uint64_t clientid, uint32_t sequence, std::string principal,
    const nfsv4::ChannelAttrs& fore_req, const nfsv4::ChannelAttrs& back_req,
    uint64_t conn_id) {
  CreateSessionResult out;
  if ((clientid >> 32) != cfg_.boot_epoch) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  std::shared_ptr<ClientRec> client;
  for (auto& shard : clients_) {  // clientid lives in its owner-hash shard: scan
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it != shard.by_id.end()) {
      client = it->second;
      break;
    }
  }
  if (!client) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  ClientShard& shard = clients_[fnv64(client->owner_id) % kShards];
  std::vector<SessionId> orphaned;
  {
    auto lock = co_await shard.mu.lock();
    // Principal collision only matters before confirmation (RFC 8881 §18.36; a
    // confirmed client may create further sessions under new credentials).
    if (!client->confirmed && client->principal != principal) {
      out.status = as_u32(Status::kClidInuse);
      co_return out;
    }
    if (sequence + 1 == client->cs_sequence && client->confirmed) {  // replay
      out.replay = true;
      out.cached = client->cs_cached_reply;
      co_return out;
    }
    if (sequence != client->cs_sequence) {
      out.status = as_u32(Status::kSeqMisordered);
      co_return out;
    }
    if (!client->confirmed) {  // confirmation: this incarnation replaces the old one
      OwnerSlot& slot = shard.by_owner[client->owner_id];
      if (slot.confirmed && slot.confirmed != client) {
        orphaned = slot.confirmed->sessions;
        shard.by_id.erase(slot.confirmed->clientid);
        client_count_.fetch_sub(1, std::memory_order_relaxed);
      }
      slot.confirmed = client;
      if (slot.unconfirmed == client) slot.unconfirmed.reset();
      client->confirmed = true;
    }

    auto session = std::make_shared<SessionRec>();
    uint32_t counter = next_session_.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(session->id.data(), &client->clientid, 8);
    std::memcpy(session->id.data() + 8, &counter, 4);
    uint32_t epoch32 = static_cast<uint32_t>(cfg_.boot_epoch);
    std::memcpy(session->id.data() + 12, &epoch32, 4);
    session->client = client;
    session->fore = fore_req;
    session->fore.headerpad = 0;
    session->fore.max_request = std::min(fore_req.max_request, cfg_.max_io);
    session->fore.max_response = std::min(fore_req.max_response, cfg_.max_io);
    session->fore.max_response_cached =
        std::min(fore_req.max_response_cached, cfg_.max_cached_reply);
    session->fore.max_ops = std::min(fore_req.max_ops, cfg_.max_ops);
    session->fore.max_requests = std::clamp(fore_req.max_requests, 1u, cfg_.max_slots);
    session->back = back_req;  // accepted verbatim; the backchannel is never used (7.7)
    session->slots.resize(session->fore.max_requests);
    session->bound_conns.insert(conn_id);

    client->cs_sequence = sequence + 1;
    client->sessions.push_back(session->id);
    renew(*client);
    out.sessionid = session->id;
    out.fore = session->fore;
    out.back = session->back;
    out.sequence = sequence;

    SessionShard& sshard = session_shard(session->id);
    // Different lock domain: release the client shard before touching sessions.
    lock.reset();
    auto slock = co_await sshard.mu.lock();
    sshard.table[session->id] = std::move(session);
    session_count_.fetch_add(1, std::memory_order_relaxed);
  }
  for (const auto& id : orphaned) {  // old incarnation's sessions die now
    SessionShard& sshard = session_shard(id);
    auto slock = co_await sshard.mu.lock();
    if (sshard.table.erase(id))
      session_count_.fetch_sub(1, std::memory_order_relaxed);
    sshard.cv.notify_all();
  }
  co_return out;
}

rt::Task<void> StateMgr::confirm_create_session(uint64_t clientid,
                                                std::vector<std::byte> reply) {
  for (auto& shard : clients_) {
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it == shard.by_id.end()) continue;
    auto& rec = *it->second;
    rec.cs_cached_reply = std::move(reply);
    if (!rec.persisted) {
      rec.persisted = true;
      persist_client(rec);  // small state_dir write; acceptable under this shard lock
    }
    co_return;
  }
}

rt::Task<uint32_t> StateMgr::destroy_session(const SessionId& id, uint64_t conn_id) {
  std::shared_ptr<SessionRec> session;
  {
    SessionShard& shard = session_shard(id);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(id);
    if (it == shard.table.end()) co_return as_u32(Status::kBadsession);
    if (!it->second->bound_conns.contains(conn_id))
      co_return as_u32(Status::kConnNotBound);
    session = std::move(it->second);
    shard.table.erase(it);
    session_count_.fetch_sub(1, std::memory_order_relaxed);
    shard.cv.notify_all();  // wake any in-flight duplicate waiters
  }
  ClientShard& cshard = clients_[fnv64(session->client->owner_id) % kShards];
  auto lock = co_await cshard.mu.lock();
  auto& list = session->client->sessions;
  list.erase(std::remove(list.begin(), list.end(), id), list.end());
  co_return 0;
}

rt::Task<uint32_t> StateMgr::destroy_clientid(uint64_t clientid) {
  for (auto& shard : clients_) {
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it == shard.by_id.end()) continue;
    if (!it->second->sessions.empty()) co_return as_u32(Status::kClientidBusy);
    auto rec = it->second;
    unpersist_client(*rec);
    auto slot_it = shard.by_owner.find(rec->owner_id);
    if (slot_it != shard.by_owner.end()) {
      if (slot_it->second.confirmed == rec) slot_it->second.confirmed.reset();
      if (slot_it->second.unconfirmed == rec) slot_it->second.unconfirmed.reset();
      if (!slot_it->second.confirmed && !slot_it->second.unconfirmed)
        shard.by_owner.erase(slot_it);
    }
    shard.by_id.erase(it);
    client_count_.fetch_sub(1, std::memory_order_relaxed);
    co_return 0;
  }
  co_return as_u32(Status::kStaleClientid);
}

rt::Task<uint32_t> StateMgr::bind_conn(const SessionId& id, uint64_t conn_id) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return as_u32(Status::kBadsession);
  it->second->bound_conns.insert(conn_id);
  co_return 0;
}

// ---- SEQUENCE --------------------------------------------------------------

rt::Task<StateMgr::SeqResult> StateMgr::sequence_begin(const SessionId& id,
                                                       uint32_t slotid, uint32_t seqid,
                                                       uint32_t highest, bool cachethis,
                                                       uint64_t conn_id) {
  (void)cachethis;
  SeqResult out;
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  for (;;) {
    auto it = shard.table.find(id);
    if (it == shard.table.end()) {
      out.status = as_u32(Status::kBadsession);
      co_return out;
    }
    SessionRec& session = *it->second;
    session.bound_conns.insert(conn_id);  // implicit bind (trunking-lenient)
    if (slotid >= session.slots.size()) {
      out.status = as_u32(Status::kBadslot);
      co_return out;
    }
    if (highest >= session.slots.size()) {
      out.status = as_u32(Status::kBadHighSlot);
      co_return out;
    }
    Slot& slot = session.slots[slotid];
    if (slot.in_flight) {
      if (seqid == slot.seqid + 1) {
        // Retransmission of the request being executed: wait, then re-classify.
        seq_waits_.fetch_add(1, std::memory_order_relaxed);
        co_await shard.cv.wait(shard.mu, lock);
        continue;
      }
      out.status = as_u32(Status::kSeqMisordered);
      co_return out;
    }
    if (seqid == slot.seqid + 1) {  // new request: claim the slot
      slot.in_flight = true;
      renew(*session.client);
      out.max_ops = session.fore.max_ops;
      out.max_request = session.fore.max_request;
      out.max_response = session.fore.max_response;
      out.max_response_cached = session.fore.max_response_cached;
      out.highest_slot = static_cast<uint32_t>(session.slots.size() - 1);
      seq_new_.fetch_add(1, std::memory_order_relaxed);
      co_return out;
    }
    if (seqid == slot.seqid && slot.seqid != 0) {  // replay
      out.replay = true;
      if (slot.cached) {
        out.replay_bytes = slot.reply;
      } else {
        out.status = as_u32(Status::kRetryUncachedRep);
      }
      renew(*session.client);
      seq_replay_.fetch_add(1, std::memory_order_relaxed);
      co_return out;
    }
    seq_misordered_.fetch_add(1, std::memory_order_relaxed);
    out.status = as_u32(Status::kSeqMisordered);
    co_return out;
  }
}

rt::Task<void> StateMgr::sequence_abort(const SessionId& id, uint32_t slotid) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return;
  if (slotid < it->second->slots.size()) it->second->slots[slotid].in_flight = false;
  shard.cv.notify_all();
}

rt::Task<void> StateMgr::sequence_complete(const SessionId& id, uint32_t slotid,
                                           uint32_t seqid, bool cache,
                                           std::vector<std::byte> reply) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return;  // destroyed mid-flight
  SessionRec& session = *it->second;
  if (slotid >= session.slots.size()) co_return;
  Slot& slot = session.slots[slotid];
  slot.in_flight = false;
  slot.seqid = seqid;
  slot.cached = cache;
  slot.reply = cache ? std::move(reply) : std::vector<std::byte>{};
  shard.cv.notify_all();
}

rt::Task<uint32_t> StateMgr::reclaim_complete(uint64_t clientid) {
  for (auto& shard : clients_) {
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it == shard.by_id.end()) continue;
    if (it->second->reclaim_complete) co_return as_u32(Status::kCompleteAlready);
    it->second->reclaim_complete = true;
    std::string owner = it->second->owner_id;
    lock.reset();
    note_reclaimed(owner);
    co_return 0;
  }
  co_return as_u32(Status::kStaleClientid);
}

// ---- minimal open-state ----------------------------------------------------

rt::Task<Stateid> StateMgr::open_read(uint64_t clientid, uint32_t fsid,
                                      const backend::ObjId& oid) {
  Stateid sid;
  sid.seqid = 1;
  // other = {boot_epoch(4B) | type(1B)=kOpen | counter(7B)} (design 07 §7.1)
  uint32_t epoch32 = static_cast<uint32_t>(cfg_.boot_epoch);
  std::memcpy(sid.other.data(), &epoch32, 4);
  sid.other[4] = std::byte{1};  // kOpen
  uint64_t counter = next_state_.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(sid.other.data() + 5, &counter, 7);
  StateShard& shard = state_shard(sid.other);
  auto lock = co_await shard.mu.lock();
  shard.table[sid.other] = OpenRec{clientid, fsid, oid, 1};
  open_count_.fetch_add(1, std::memory_order_relaxed);
  co_return sid;
}

rt::Task<StateMgr::StateLookup> StateMgr::lookup_stateid(const Stateid& sid) {
  StateLookup out;
  if (sid.is_special()) {
    out.special = true;
    co_return out;
  }
  uint32_t epoch32 = 0;
  std::memcpy(&epoch32, sid.other.data(), 4);
  if (epoch32 != static_cast<uint32_t>(cfg_.boot_epoch)) {
    out.status = as_u32(Status::kStaleStateid);  // pre-restart stateid: no table walk
    co_return out;
  }
  StateShard& shard = state_shard(sid.other);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(sid.other);
  if (it == shard.table.end()) out.status = as_u32(Status::kBadStateid);
  else out.rec = it->second;
  co_return out;
}

rt::Task<uint32_t> StateMgr::close_state(const Stateid& sid) {
  if (sid.is_special()) co_return as_u32(Status::kBadStateid);
  uint32_t epoch32 = 0;
  std::memcpy(&epoch32, sid.other.data(), 4);
  if (epoch32 != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateShard& shard = state_shard(sid.other);
  auto lock = co_await shard.mu.lock();
  bool erased = shard.table.erase(sid.other) != 0;
  if (erased) open_count_.fetch_sub(1, std::memory_order_relaxed);
  co_return erased ? 0 : as_u32(Status::kBadStateid);
}

rt::Task<uint32_t> StateMgr::free_stateid(const Stateid& sid) {
  co_return co_await close_state(sid);
}

StateMgr::Stats StateMgr::stats() const {
  Stats out;
  out.clients = static_cast<size_t>(std::max<int64_t>(0, client_count_.load(std::memory_order_relaxed)));
  out.sessions = static_cast<size_t>(std::max<int64_t>(0, session_count_.load(std::memory_order_relaxed)));
  out.opens = static_cast<size_t>(std::max<int64_t>(0, open_count_.load(std::memory_order_relaxed)));
  out.seq_new = seq_new_.load(std::memory_order_relaxed);
  out.seq_replay = seq_replay_.load(std::memory_order_relaxed);
  out.seq_misordered = seq_misordered_.load(std::memory_order_relaxed);
  out.seq_waits = seq_waits_.load(std::memory_order_relaxed);
  out.grace = in_grace();
  return out;
}

}  // namespace lnfs::state
