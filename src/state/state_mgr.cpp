#include "state/state_mgr.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <utility>

#include "runtime/io.hpp"
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

uint32_t epoch_of(const StateOther& other) {
  uint32_t epoch32 = 0;
  std::memcpy(&epoch32, other.data(), 4);
  return epoch32;
}

// Share reservation conflict (RFC 8881 §18.16.3): my access vs. their deny and my
// deny vs. their access.
bool share_conflict(uint32_t access, uint32_t deny, const StateRec& other) {
  uint32_t o_access = other.access, o_deny = other.deny;
  return (access & o_deny) != 0 || (deny & o_access) != 0;
}

constexpr int kReasonForced = 0, kReasonConflict = 1, kReasonTimeout = 2,
              kReasonReboot = 3;

std::string hex_other(const StateOther& other) {
  std::string out;
  for (std::byte b : other) out += std::format("{:02x}", static_cast<unsigned>(b));
  return out;
}

}  // namespace

size_t StateMgr::SessionIdHash::operator()(const SessionId& id) const noexcept {
  uint64_t h;
  std::memcpy(&h, id.data(), 8);
  uint64_t l;
  std::memcpy(&l, id.data() + 8, 8);
  return static_cast<size_t>(h ^ (l * 1099511628211ull));
}

size_t OtherHash::operator()(const StateOther& other) const noexcept {
  uint64_t h;
  std::memcpy(&h, other.data(), 8);
  uint32_t l;
  std::memcpy(&l, other.data() + 8, 4);
  return static_cast<size_t>(h ^ (uint64_t(l) << 17));
}

StateMgr::StateMgr(Config cfg)
    : cfg_(std::move(cfg)),
      shard_count_(std::max<uint32_t>(1, cfg_.shards)),
      clients_(std::make_unique<ClientShard[]>(shard_count_)),
      sessions_(std::make_unique<SessionShard[]>(shard_count_)),
      states_(std::make_unique<StateShard[]>(shard_count_)),
      files_(std::make_unique<FileShard[]>(shard_count_)) {}

StateMgr::ClientShard& StateMgr::owner_shard(std::string_view owner_id) {
  return clients_[fnv64(owner_id) % shard_count_];
}
StateMgr::SessionShard& StateMgr::session_shard(const SessionId& id) {
  return sessions_[SessionIdHash{}(id) % shard_count_];
}
StateMgr::StateShard& StateMgr::state_shard(const StateOther& other) {
  return states_[OtherHash{}(other) % shard_count_];
}
StateMgr::FileShard& StateMgr::file_shard(const FileKey& key) {
  return files_[FileKeyHash{}(key) % shard_count_];
}

void StateMgr::index_client(const std::shared_ptr<ClientRec>& rec) {
  std::lock_guard g(client_idx_mu_);
  client_idx_[rec->clientid] = rec;
}
void StateMgr::unindex_client(uint64_t clientid) {
  std::lock_guard g(client_idx_mu_);
  client_idx_.erase(clientid);
}

std::shared_ptr<ClientRec> StateMgr::find_client_sync(uint64_t clientid) {
  if ((clientid >> 32) != cfg_.boot_epoch) return nullptr;
  std::lock_guard g(client_idx_mu_);
  auto it = client_idx_.find(clientid);
  return it == client_idx_.end() ? nullptr : it->second;
}

void StateMgr::arm_lease_check(ClientRec& client, int64_t when) {
  std::lock_guard g(lease_heap_mu_);
  // An earlier live entry pops first and re-arms; only advance towards `when`.
  if (client.lease_heap_deadline >= 0 && client.lease_heap_deadline <= when) return;
  client.lease_heap_deadline = when;
  lease_heap_.emplace(when, client.clientid);
}

void StateMgr::wake_all_slots(SessionRec& session) {
  for (uint32_t i = 0; i < session.slot_count; ++i) session.slots[i].cv.notify_all();
}

int64_t StateMgr::now_coarse() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void StateMgr::renew(ClientRec& client) {
  // 07 §7.2: the SEQUENCE fast path only does an atomic store; the scanner reads lazily.
  int64_t expiry = now_coarse() + cfg_.lease_seconds;
  client.lease_expiry.store(expiry, std::memory_order_relaxed);
  // A courtesy client that comes back before reclaim revives with its state intact
  // (RFC 8881 §8.4.2 server discretion; same policy as Linux nfsd).
  if (client.courtesy.exchange(false, std::memory_order_relaxed)) {
    courtesy_count_.fetch_sub(1, std::memory_order_relaxed);
    // Its heap entry sits at the (distant) courtesy deadline: re-arm at the fresh
    // lease expiry so a renewed-then-vanished client is noticed on time.
    arm_lease_check(client, expiry);
  }
}

StateOther StateMgr::new_other(StateType type) {
  // other = {boot_epoch(4B) | type(1B) | counter(7B)} (design 07 §7.1)
  StateOther other{};
  uint32_t epoch32 = static_cast<uint32_t>(cfg_.boot_epoch);
  std::memcpy(other.data(), &epoch32, 4);
  other[4] = static_cast<std::byte>(type);
  uint64_t counter = next_state_.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(other.data() + 5, &counter, 7);
  return other;
}

rt::Task<std::shared_ptr<ClientRec>> StateMgr::find_client(uint64_t clientid) {
  co_return find_client_sync(clientid);
}

// ---- grace -----------------------------------------------------------------

// The single-gateway stable store: one file per client under state_dir/clients/,
// named by fnv64(owner), holding the co_ownerid verbatim.
std::vector<std::string> StateMgr::load_local_clients() const {
  std::vector<std::string> owners;
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
    if (!owner.empty()) owners.push_back(std::move(owner));
  }
  return owners;
}

void StateMgr::load_grace_list() {
  std::vector<std::string> owners = cfg_.stable.load ? cfg_.stable.load() : load_local_clients();
  std::lock_guard lock(grace_mu_);
  for (auto& owner : owners) {
    if (owner.empty()) continue;
    stable_list_.insert(owner);
    grace_pending_.insert(std::move(owner));
  }
  uint32_t grace_secs = cfg_.grace_seconds ? cfg_.grace_seconds : cfg_.lease_seconds;
  if (!grace_pending_.empty()) {
    grace_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(grace_secs);
    grace_active_.store(true, std::memory_order_relaxed);
    LNFS_INFO("grace period armed: {} clients expected to reclaim, {}s window",
              grace_pending_.size(), grace_secs);
  }
}

bool StateMgr::end_grace() {
  std::lock_guard lock(grace_mu_);
  grace_pending_.clear();
  bool was = grace_active_.exchange(false, std::memory_order_relaxed);
  if (was) LNFS_INFO("grace period ended by operator (grace-end)");
  return was;
}

bool StateMgr::in_grace() const {
  if (!grace_active_.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(grace_mu_);
  if (grace_pending_.empty() || std::chrono::steady_clock::now() >= grace_deadline_) {
    if (grace_active_.exchange(false, std::memory_order_relaxed))
      LNFS_INFO("grace period over");
    return false;
  }
  return true;
}

int64_t StateMgr::grace_remaining_seconds() const {
  if (!in_grace()) return 0;
  std::lock_guard lock(grace_mu_);
  auto left = grace_deadline_ - std::chrono::steady_clock::now();
  return std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(left).count());
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
  if (cfg_.stable.put) {
    cfg_.stable.put(client.owner_id);
    return;
  }
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
  if (cfg_.stable.erase) {
    cfg_.stable.erase(client.owner_id);
    return;
  }
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
  ClientShard& shard = owner_shard(owner_id);
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
      if (!c.sessions.empty() || !c.states.empty()) {  // held state: owner is taken
        out.status = as_u32(Status::kClidInuse);
        co_return out;
      }
      // RFC 8881 §18.35.4 case 3: no state under the old principal — remove the old
      // record and register the new principal's incarnation from scratch.
      unpersist_client(c);
      shard.by_id.erase(c.clientid);
      unindex_client(c.clientid);
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
    unindex_client(slot.unconfirmed->clientid);
    client_count_.fetch_sub(1, std::memory_order_relaxed);
    slot.unconfirmed.reset();
  }
  // Client cap (plan doc 10 §1.5): refuse registrations past the limit instead of
  // letting a churning client id flood grow the tables without bound.
  if (cfg_.max_clients != 0 &&
      client_count_.load(std::memory_order_relaxed) >=
          static_cast<int64_t>(cfg_.max_clients)) {
    if (!slot.confirmed) shard.by_owner.erase(owner_id);
    out.status = as_u32(Status::kResource);
    co_return out;
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
  index_client(rec);
  arm_lease_check(*rec, rec->lease_expiry.load(std::memory_order_relaxed));
  client_count_.fetch_add(1, std::memory_order_relaxed);
  out.clientid = rec->clientid;
  out.sequenceid = rec->cs_sequence;
  co_return out;
}

// ---- CREATE_SESSION --------------------------------------------------------

rt::Task<StateMgr::CreateSessionResult> StateMgr::create_session(
    uint64_t clientid, uint32_t sequence, std::string principal,
    const nfsv4::ChannelAttrs& fore_req, const nfsv4::ChannelAttrs& back_req,
    uint64_t conn_id, std::shared_ptr<transport::CbChannel> cb_chan, uint32_t cb_program,
    nfsv4::cb::Cred cb_cred) {
  CreateSessionResult out;
  auto client = co_await find_client(clientid);
  if (!client) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  ClientShard& shard = owner_shard(client->owner_id);
  std::shared_ptr<ClientRec> old_incarnation;  // client reboot: its state dies (§4.8)
  std::vector<SessionId> orphaned;
  std::vector<StateOther> orphaned_states;
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
        old_incarnation = slot.confirmed;
        old_incarnation->expired.store(true, std::memory_order_relaxed);
        if (old_incarnation->courtesy.exchange(false, std::memory_order_relaxed))
          courtesy_count_.fetch_sub(1, std::memory_order_relaxed);
        orphaned = std::move(old_incarnation->sessions);
        orphaned_states.assign(old_incarnation->states.begin(),
                               old_incarnation->states.end());
        old_incarnation->states.clear();
        shard.by_id.erase(old_incarnation->clientid);
        unindex_client(old_incarnation->clientid);
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
    // Backchannel (plan doc 10 §5.2): clamp to the one slot we serialize callbacks
    // on; the channel itself attaches here (CONN_BACK_CHAN) or via BIND_CONN.
    session->back = back_req;
    session->back.max_requests = 1;
    session->back.max_ops = 2;  // CB_SEQUENCE + one op is all we ever send
    session->cb_chan = std::move(cb_chan);
    session->cb_program = cb_program;
    session->cb_cred = std::move(cb_cred);
    session->slot_count = session->fore.max_requests;
    session->slots = std::make_unique<Slot[]>(session->slot_count);
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
    auto sit = sshard.table.find(id);
    if (sit != sshard.table.end()) {
      auto dead = std::move(sit->second);
      sshard.table.erase(sit);
      session_count_.fetch_sub(1, std::memory_order_relaxed);
      wake_all_slots(*dead);  // before the last reference drops
    }
  }
  if (old_incarnation) {  // ...and so does its open state (RFC 8881 §8.4.2 / notes 4.8)
    std::vector<backend::OpenPtr> released;
    for (const auto& other : orphaned_states) {
      StateRef rec;
      {
        StateShard& sshard = state_shard(other);
        auto slock = co_await sshard.mu.lock();
        auto it = sshard.table.find(other);
        if (it != sshard.table.end()) rec = it->second;
      }
      if (rec) released.push_back(co_await unlink_state(rec, false));
    }
    LNFS_INFO("client {:#x} rebooted: released {} open states of the old incarnation",
              old_incarnation->clientid, orphaned_states.size());
    released.clear();  // backend handles drop here, outside every shard lock
  }
  co_return out;
}

rt::Task<void> StateMgr::confirm_create_session(uint64_t clientid,
                                                std::vector<std::byte> reply) {
  std::shared_ptr<ClientRec> to_persist;
  if (auto client = find_client_sync(clientid)) {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    if (shard.by_id.contains(clientid)) {
      client->cs_cached_reply = std::move(reply);
      if (!client->persisted) {
        client->persisted = true;  // claimed under the lock; the write happens outside
        to_persist = client;
      }
    }
  }
  // open+write+fsync must not run under a shard lock (invariant at the top of
  // state_mgr.hpp; plan doc 10 §1.7).  owner_id is immutable after registration.
  if (to_persist) persist_client(*to_persist);
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
    wake_all_slots(*session);  // wake any in-flight duplicate waiters
  }
  ClientShard& cshard = owner_shard(session->client->owner_id);
  auto lock = co_await cshard.mu.lock();
  auto& list = session->client->sessions;
  list.erase(std::remove(list.begin(), list.end(), id), list.end());
  co_return 0;
}

rt::Task<uint32_t> StateMgr::destroy_clientid(uint64_t clientid) {
  auto rec = find_client_sync(clientid);
  if (!rec) co_return as_u32(Status::kStaleClientid);
  {
    ClientShard& shard = owner_shard(rec->owner_id);
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it == shard.by_id.end() || it->second != rec)
      co_return as_u32(Status::kStaleClientid);  // raced with expiry/replacement
    if (!rec->sessions.empty() || !rec->states.empty())
      co_return as_u32(Status::kClientidBusy);
    unpersist_client(*rec);
    auto slot_it = shard.by_owner.find(rec->owner_id);
    if (slot_it != shard.by_owner.end()) {
      if (slot_it->second.confirmed == rec) slot_it->second.confirmed.reset();
      if (slot_it->second.unconfirmed == rec) slot_it->second.unconfirmed.reset();
      if (!slot_it->second.confirmed && !slot_it->second.unconfirmed)
        shard.by_owner.erase(slot_it);
    }
    if (rec->courtesy.exchange(false, std::memory_order_relaxed))
      courtesy_count_.fetch_sub(1, std::memory_order_relaxed);
    shard.by_id.erase(it);
    unindex_client(clientid);
    client_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  std::lock_guard g(lock_owner_mu_);
  std::erase_if(lock_owners_, [&](const auto& kv) {
    return kv.second.client->clientid == clientid;
  });
  co_return 0;
}

rt::Task<uint32_t> StateMgr::bind_conn(const SessionId& id, uint64_t conn_id) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return as_u32(Status::kBadsession);
  it->second->bound_conns.insert(conn_id);
  co_return 0;
}

rt::Task<uint32_t> StateMgr::bind_backchannel(const SessionId& id,
                                              std::shared_ptr<transport::CbChannel> chan) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return as_u32(Status::kBadsession);
  SessionRec& session = *it->second;
  {
    std::lock_guard cb(session.cb_mu);
    session.cb_chan = std::move(chan);
    session.cb_inflight = false;
  }
  session.cb_down.store(false, std::memory_order_relaxed);  // fresh path, fresh chance
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
    if (it == shard.table.end() ||
        it->second->client->expired.load(std::memory_order_relaxed)) {
      out.status = as_u32(Status::kBadsession);  // gone, or reclaim chain in progress
      co_return out;
    }
    SessionRec& session = *it->second;
    session.bound_conns.insert(conn_id);  // implicit bind (trunking-lenient)
    if (slotid >= session.slot_count) {
      out.status = as_u32(Status::kBadslot);
      co_return out;
    }
    if (highest >= session.slot_count) {
      out.status = as_u32(Status::kBadHighSlot);
      co_return out;
    }
    Slot& slot = session.slots[slotid];
    if (slot.in_flight) {
      if (seqid == slot.seqid + 1) {
        // Retransmission of the request being executed: wait on this slot, then
        // re-classify (plan doc 10 §2.6: completing another slot in the shard no
        // longer wakes us).
        seq_waits_.fetch_add(1, std::memory_order_relaxed);
        co_await slot.cv.wait(shard.mu, lock);
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
      out.highest_slot = session.slot_count - 1;
      // SEQ4_STATUS_CB_PATH_DOWN (plan doc 10 §5.2): the client holds recallable
      // state but this session has no usable backchannel — it should re-bind one.
      if (session.client->delegs.load(std::memory_order_relaxed) > 0) {
        std::lock_guard cb(session.cb_mu);
        bool cb_alive = session.cb_chan && session.cb_chan->alive() &&
                        !session.cb_down.load(std::memory_order_relaxed);
        if (!cb_alive) out.status_flags |= 0x1;
      }
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
  if (slotid < it->second->slot_count) {
    it->second->slots[slotid].in_flight = false;
    it->second->slots[slotid].cv.notify_all();
  }
}

rt::Task<void> StateMgr::sequence_complete(const SessionId& id, uint32_t slotid,
                                           uint32_t seqid, bool cache,
                                           std::vector<std::byte> reply) {
  SessionShard& shard = session_shard(id);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(id);
  if (it == shard.table.end()) co_return;  // destroyed mid-flight
  SessionRec& session = *it->second;
  if (slotid >= session.slot_count) co_return;
  Slot& slot = session.slots[slotid];
  slot.in_flight = false;
  slot.seqid = seqid;
  slot.cached = cache;
  slot.reply = cache ? std::move(reply) : std::vector<std::byte>{};
  slot.cv.notify_all();
}

rt::Task<uint32_t> StateMgr::reclaim_complete(uint64_t clientid) {
  auto client = find_client_sync(clientid);
  if (!client) co_return as_u32(Status::kStaleClientid);
  ClientShard& shard = owner_shard(client->owner_id);
  {
    auto lock = co_await shard.mu.lock();
    if (!shard.by_id.contains(clientid)) co_return as_u32(Status::kStaleClientid);
    if (client->reclaim_complete) co_return as_u32(Status::kCompleteAlready);
    client->reclaim_complete = true;
  }
  note_reclaimed(client->owner_id);
  co_return 0;
}

// ---- open state ------------------------------------------------------------

rt::Task<backend::OpenPtr> StateMgr::unlink_state(const StateRef& rec, bool from_client) {
  backend::OpenPtr released;
  {
    StateShard& shard = state_shard(rec->other);
    auto lock = co_await shard.mu.lock();
    if (rec->closed) co_return nullptr;  // raced with CLOSE / another reclaim
    rec->closed = true;
    shard.table.erase(rec->other);
    if (rec->type == StateType::kOpen)
      open_count_.fetch_sub(1, std::memory_order_relaxed);
    released = std::move(rec->bopen);  // under the state shard: IO-path copies are safe
  }
  FileKey key{rec->fsid, rec->oid};
  std::vector<StateRef> dependents;  // lock states derived from a closing open
  {
    FileShard& shard = file_shard(key);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(key);
    if (it != shard.table.end()) {
      auto& file = it->second;
      if (rec->type == StateType::kOpen) {
        auto& opens = file.opens;
        opens.erase(std::remove(opens.begin(), opens.end(), rec), opens.end());
        for (const auto& l : file.locks)
          if (l->parent_open == rec) dependents.push_back(l);
      } else if (rec->type == StateType::kLock) {
        auto& locks = file.locks;
        locks.erase(std::remove(locks.begin(), locks.end(), rec), locks.end());
      } else {
        auto& delegs = file.delegs;
        delegs.erase(std::remove(delegs.begin(), delegs.end(), rec), delegs.end());
      }
      if (file.opens.empty() && file.locks.empty() && file.delegs.empty()) {
        shard.table.erase(it);
        file_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
  if (rec->type == StateType::kLock) {
    locks_.release_owner(key, rec->lowner);  // plain mutex table: no shard lock held
    if (backend::LockMgr* native = native_lock_mgr(rec->fsid)) {
      // Drop the owner's native locks too (CLOSE / FREE_STATEID / expiry).  A file
      // that is already gone cannot be resolved; its descriptor is closed when the
      // backend stops (the locks die with the inode anyway).
      auto obj = co_await native_lock_object(rec->fsid, rec->oid);
      Result<void> released = obj ? co_await native->release(**obj, rec->lowner)
                                  : Result<void>(Err(obj.error()));
      if (!released) {
        native_lock_errors_.fetch_add(1, std::memory_order_relaxed);
        LNFS_WARN("native lock release failed (fsid {}): {}", rec->fsid,
                  errno_name(released.error()));
      }
    }
    lock_count_.fetch_sub(1, std::memory_order_relaxed);
    // The freed ranges may unblock waiters (plan doc 10 §5.2): CB_NOTIFY_LOCK them.
    notify_lock_waiters(key);
  }
  if (rec->type == StateType::kDeleg) {
    deleg_count_.fetch_sub(1, std::memory_order_relaxed);
    rec->client->delegs.fetch_sub(1, std::memory_order_relaxed);
  }
  // CLOSE releases the byte-range locks held through this open (RFC 8881 §18.2.4).
  for (const auto& l : dependents) (void)co_await unlink_state(l, from_client);
  if (from_client) {
    ClientShard& shard = owner_shard(rec->client->owner_id);
    auto lock = co_await shard.mu.lock();
    rec->client->states.erase(rec->other);
  }
  co_return released;  // caller drops it with no lock held
}

rt::Task<StateMgr::OpenResult> StateMgr::open(OpenArgs args, backend::OpenPtr bopen) {
  OpenResult out;
  auto client = co_await find_client(args.clientid);
  if (!client || !client->confirmed) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  if (client->expired.load(std::memory_order_relaxed)) {
    out.status = as_u32(Status::kExpired);
    co_return out;
  }
  // Grace gate (07 §7.5): reclaims only from listed clients that have not finished
  // reclaiming; every other state-creating OPEN waits the grace period out.  A client
  // that never sent RECLAIM_COMPLETE may not create non-reclaim state at all
  // (RFC 8881 §18.51.3), grace or not.
  bool reclaim_done;
  size_t held_states;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    reclaim_done = client->reclaim_complete;
    held_states = client->states.size();
  }
  if (args.reclaim) {
    if (!in_grace()) {
      out.status = as_u32(Status::kNoGrace);
      co_return out;
    }
    if (!in_stable_list(client->owner_id)) {
      out.status = as_u32(Status::kReclaimBad);
      co_return out;
    }
    if (reclaim_done) {
      out.status = as_u32(Status::kNoGrace);
      co_return out;
    }
  } else if (in_grace() || !reclaim_done) {
    out.status = as_u32(Status::kGrace);
    co_return out;
  }
  // Per-client state cap (plan doc 10 §1.5).  Checked before arbitration, so a client
  // sitting exactly at the cap is refused even a same-owner merge — acceptable for a
  // limit this size, and it keeps the check outside the file-shard critical section.
  if (cfg_.max_states_per_client != 0 && held_states >= cfg_.max_states_per_client) {
    out.status = as_u32(Status::kResource);
    co_return out;
  }

  FileKey key{args.fsid, args.oid};
  StateRef rec;
  backend::OpenPtr released;
  std::vector<StateRef> deleg_recalls;  // write open vs. read delegations (§5.2)
  for (int attempt = 0;; ++attempt) {
    uint64_t courtesy_conflict = 0;
    {
      FileShard& shard = file_shard(key);
      auto lock = co_await shard.mu.lock();
      auto& file = shard.table[key];
      StateRef mine;
      bool denied = false;
      for (const auto& other : file.opens) {
        if (other->closed || other->client->expired.load(std::memory_order_relaxed))
          continue;  // dead entries awaiting unlink never arbitrate
        if (other->client == client && other->owner == args.owner) {
          mine = other;
          continue;
        }
        if (share_conflict(args.access, args.deny, *other)) {
          if (other->client->courtesy.load(std::memory_order_relaxed)) {
            courtesy_conflict = other->client->clientid;  // conflict reclaims it (7.4)
            break;
          }
          denied = true;
          break;
        }
      }
      // A write open invalidates every read delegation on the file, the opener's own
      // included (plan doc 10 §5.2): recall them and let the client retry (DELAY).
      // A DELEG_CUR_FH claim is the recall response itself and passes through.
      if (courtesy_conflict == 0 && !denied && (args.access & kShareWrite) &&
          !args.deleg_claim) {
        for (const auto& d : file.delegs)
          if (!d->closed && !d->client->expired.load(std::memory_order_relaxed))
            deleg_recalls.push_back(d);
      }
      if (!deleg_recalls.empty()) {
        // Nothing created; fall out of the lock scope to start the recalls.
      } else if (courtesy_conflict == 0) {
        if (denied) {
          if (file.opens.empty() && file.locks.empty() && file.delegs.empty())
            shard.table.erase(key);
          share_denied_.fetch_add(1, std::memory_order_relaxed);
          out.status = as_u32(Status::kShareDenied);
          co_return out;
        }
        if (mine) {  // same owner + file: union, seqid++ (RFC 8881 §18.16.3)
          mine->access |= args.access;
          mine->deny |= args.deny;
          mine->seqid += 1;
          released = std::move(bopen);  // the record keeps its original handle
          rec = mine;
          out.merged = true;
          open_merges_.fetch_add(1, std::memory_order_relaxed);
        } else {
          rec = std::make_shared<StateRec>();
          rec->other = new_other(StateType::kOpen);
          rec->client = client;
          rec->fsid = args.fsid;
          rec->oid = args.oid;
          rec->owner = std::move(args.owner);
          rec->access = args.access;
          rec->deny = args.deny;
          rec->bopen = std::move(bopen);
          if (file.opens.empty()) file_count_.fetch_add(1, std::memory_order_relaxed);
          file.opens.push_back(rec);
        }
      } else if (file.opens.empty()) {
        shard.table.erase(key);
      }
    }
    if (!deleg_recalls.empty()) {
      for (const auto& d : deleg_recalls) start_recall(d);
      out.status = as_u32(Status::kDelay);
      co_return out;
    }
    if (courtesy_conflict == 0) break;
    if (attempt >= 8) {  // pathological: keep the invariant rather than spin
      out.status = as_u32(Status::kDelay);
      co_return out;
    }
    (void)co_await expire_client_impl(courtesy_conflict, kReasonConflict);
  }

  if (!out.merged) {
    {
      StateShard& shard = state_shard(rec->other);
      auto lock = co_await shard.mu.lock();
      shard.table[rec->other] = rec;
      open_count_.fetch_add(1, std::memory_order_relaxed);
    }
    bool dead;
    {
      ClientShard& shard = owner_shard(client->owner_id);
      auto lock = co_await shard.mu.lock();
      dead = client->expired.load(std::memory_order_relaxed);
      if (!dead) client->states.insert(rec->other);
    }
    if (dead) {  // reclaim chain ran between arbitration and registration
      released = co_await unlink_state(rec, false);
      out.status = as_u32(Status::kExpired);
      co_return out;
    }
  }
  out.stateid.seqid = rec->seqid;
  out.stateid.other = rec->other;
  released.reset();  // superseded backend handle: dropped with no lock held
  co_return out;
}

rt::Task<Stateid> StateMgr::open_read(uint64_t clientid, uint32_t fsid,
                                      const backend::ObjId& oid) {
  OpenArgs args;
  args.clientid = clientid;
  args.fsid = fsid;
  args.oid = oid;
  args.owner = "\x01legacy-read";
  args.access = kShareRead;
  auto result = co_await open(std::move(args), nullptr);
  co_return result.stateid;
}

// ---- read delegations + backchannel (plan doc 10 §5.2) ---------------------

rt::Task<StateMgr::DelegGrant> StateMgr::maybe_grant_read_deleg(
    const SessionId& sessionid, uint64_t clientid, uint32_t fsid,
    const backend::ObjId& oid, std::vector<std::byte> fh, uint32_t access,
    bool reclaim) {
  DelegGrant out;
  if (!cfg_.delegations || reclaim || access != kShareRead || in_grace()) co_return out;
  // The granting session must have a live backchannel: without one a recall could
  // never reach the client.
  {
    SessionShard& shard = session_shard(sessionid);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sessionid);
    if (it == shard.table.end()) co_return out;
    SessionRec& session = *it->second;
    std::lock_guard cb(session.cb_mu);
    if (!session.cb_chan || !session.cb_chan->alive() ||
        session.cb_down.load(std::memory_order_relaxed))
      co_return out;
  }
  auto client = co_await find_client(clientid);
  if (!client || client->expired.load(std::memory_order_relaxed)) co_return out;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    if (cfg_.max_states_per_client != 0 &&
        client->states.size() >= cfg_.max_states_per_client)
      co_return out;
  }
  auto rec = std::make_shared<StateRec>();
  rec->other = new_other(StateType::kDeleg);
  rec->type = StateType::kDeleg;
  rec->client = client;
  rec->fsid = fsid;
  rec->oid = oid;
  rec->owner = "\x01" "deleg";
  rec->access = kShareRead;
  rec->fh = std::move(fh);
  FileKey key{fsid, oid};
  {
    // One file-shard scope for the checks and the publication, so a racing write
    // open either sees this delegation (and recalls it) or blocks the grant here.
    // The caller's own open is already in file.opens: the entry exists.
    FileShard& shard = file_shard(key);
    auto lock = co_await shard.mu.lock();
    auto& file = shard.table[key];
    for (const auto& o : file.opens) {
      if (o->closed || o->client->expired.load(std::memory_order_relaxed)) continue;
      if (o->client != client &&
          (o->access.load(std::memory_order_relaxed) & kShareWrite))
        co_return out;  // live write open elsewhere: not coherent to delegate
    }
    for (const auto& d : file.delegs) {
      if (d->closed) continue;
      if (d->client == client || d->recalled.load(std::memory_order_relaxed))
        co_return out;  // one per client; nothing new while a recall is running
    }
    file.delegs.push_back(rec);
  }
  {
    StateShard& shard = state_shard(rec->other);
    auto lock = co_await shard.mu.lock();
    shard.table[rec->other] = rec;
  }
  bool dead;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    dead = client->expired.load(std::memory_order_relaxed);
    if (!dead) client->states.insert(rec->other);
  }
  client->delegs.fetch_add(1, std::memory_order_relaxed);
  deleg_count_.fetch_add(1, std::memory_order_relaxed);
  if (dead) {  // reclaim chain ran mid-grant: retract
    (void)co_await unlink_state(rec, false);
    co_return out;
  }
  deleg_grants_.fetch_add(1, std::memory_order_relaxed);
  out.granted = true;
  out.stateid.seqid = rec->seqid.load(std::memory_order_relaxed);
  out.stateid.other = rec->other;
  co_return out;
}

rt::Task<uint32_t> StateMgr::delegreturn(const Stateid& sid, uint64_t clientid,
                                         uint32_t fsid, const backend::ObjId& oid) {
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  if (rec->type != StateType::kDeleg || rec->client->clientid != clientid ||
      rec->fsid != fsid || !(rec->oid == oid))
    co_return as_u32(Status::kBadStateid);
  (void)co_await unlink_state(rec, true);
  deleg_returns_.fetch_add(1, std::memory_order_relaxed);
  co_return 0;
}

rt::Task<uint32_t> StateMgr::deleg_conflict(uint32_t fsid, const backend::ObjId& oid) {
  if (deleg_count_.load(std::memory_order_relaxed) == 0) co_return 0;  // fast path
  std::vector<StateRef> recalls;
  FileKey key{fsid, oid};
  {
    FileShard& shard = file_shard(key);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(key);
    if (it != shard.table.end()) {
      for (const auto& d : it->second.delegs)
        if (!d->closed && !d->client->expired.load(std::memory_order_relaxed))
          recalls.push_back(d);
    }
  }
  if (recalls.empty()) co_return 0;
  for (const auto& d : recalls) start_recall(d);
  co_return as_u32(Status::kDelay);
}

rt::Task<uint32_t> StateMgr::check_deleg_claim(const Stateid& sid, uint64_t clientid,
                                               uint32_t fsid, const backend::ObjId& oid) {
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  if (rec->type != StateType::kDeleg || rec->client->clientid != clientid ||
      rec->fsid != fsid || !(rec->oid == oid))
    co_return as_u32(Status::kBadStateid);
  co_return 0;
}

void StateMgr::start_recall(const StateRef& deleg) {
  if (deleg->recalled.exchange(true, std::memory_order_relaxed)) return;  // once only
  deleg->recall_deadline.store(now_coarse() + cfg_.lease_seconds,
                               std::memory_order_relaxed);
  {
    std::lock_guard lock(recall_mu_);
    recall_watch_.push_back(deleg);
  }
  deleg_recalls_.fetch_add(1, std::memory_order_relaxed);
  rt::spawn(recall_task(deleg), rt::current_reactor());
}

rt::Task<void> StateMgr::recall_task(StateRef deleg) {
  Stateid sid;
  sid.seqid = deleg->seqid.load(std::memory_order_relaxed);
  sid.other = deleg->other;
  struct Args {
    Stateid sid;
    const std::vector<std::byte>* fh;
  } args{sid, &deleg->fh};
  bool sent = co_await send_cb(
      deleg->client,
      [](const nfsv4::cb::Target& t, const void* p) {
        auto* a = static_cast<const Args*>(p);
        return nfsv4::cb::build_cb_recall(t, a->sid, *a->fh);
      },
      &args);
  if (!sent)
    LNFS_WARN("CB_RECALL undeliverable (client {:#x}): delegation will be revoked at "
              "the deadline",
              deleg->client->clientid);
}

rt::Task<bool> StateMgr::send_cb(std::shared_ptr<ClientRec> client,
                                 std::vector<std::byte> (*build)(const nfsv4::cb::Target&,
                                                                 const void*),
                                 const void* args) {
  std::vector<SessionId> session_ids;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    session_ids = client->sessions;
  }
  std::shared_ptr<SessionRec> session;
  for (const auto& id : session_ids) {
    SessionShard& shard = session_shard(id);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(id);
    if (it == shard.table.end()) continue;
    std::lock_guard cb(it->second->cb_mu);
    if (it->second->cb_chan && it->second->cb_chan->alive() &&
        !it->second->cb_down.load(std::memory_order_relaxed)) {
      session = it->second;
      break;
    }
  }
  if (!session) co_return false;
  nfsv4::cb::Target target;
  std::shared_ptr<transport::CbChannel> chan;
  {
    std::lock_guard cb(session->cb_mu);
    if (!session->cb_chan || !session->cb_chan->alive() ||
        session->cb_down.load(std::memory_order_relaxed))
      co_return false;
    int64_t now = now_coarse();
    if (session->cb_inflight) {
      // One slot, one call at a time.  A slot stuck past a lease means the client
      // stopped answering: mark the path down instead of queueing behind it.
      if (now - session->cb_inflight_since >
          static_cast<int64_t>(cfg_.lease_seconds))
        session->cb_down.store(true, std::memory_order_relaxed);
      co_return false;
    }
    session->cb_inflight = true;
    session->cb_inflight_since = now;
    chan = session->cb_chan;
    target.program = session->cb_program;
    target.cred = session->cb_cred;
    target.sessionid = session->id;
    target.slot_seq = ++session->cb_seq;
    target.xid = chan->next_xid();
  }
  auto record = build(target, args);
  auto reply = co_await chan->call(target.xid, std::move(record));
  bool ok = false;
  {
    std::lock_guard cb(session->cb_mu);
    session->cb_inflight = false;
    if (!reply) {
      session->cb_down.store(true, std::memory_order_relaxed);
    } else {
      auto rs = nfsv4::cb::parse_cb_reply(*reply);
      ok = rs.rpc_ok;  // any well-formed RPC answer proves the path is alive
      if (!ok) session->cb_down.store(true, std::memory_order_relaxed);
    }
  }
  co_return ok;
}

rt::Task<void> StateMgr::sweep_recalls() {
  std::vector<StateRef> due;
  int64_t now = now_coarse();
  {
    std::lock_guard lock(recall_mu_);
    for (size_t i = 0; i < recall_watch_.size();) {
      StateRef& rec = recall_watch_[i];
      if (rec->closed) {  // DELEGRETURN / client expiry beat the deadline
        rec = recall_watch_.back();
        recall_watch_.pop_back();
        continue;
      }
      if (rec->recall_deadline.load(std::memory_order_relaxed) <= now) {
        due.push_back(rec);
        rec = recall_watch_.back();
        recall_watch_.pop_back();
        continue;
      }
      ++i;
    }
  }
  for (auto& rec : due) {
    LNFS_WARN("delegation recall deadline passed: revoking (client {:#x})",
              rec->client->clientid);
    (void)co_await unlink_state(rec, true);
    deleg_revokes_.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- CB_NOTIFY_LOCK (plan doc 10 §5.2) -------------------------------------

void StateMgr::register_lock_waiter(uint32_t fsid, const backend::ObjId& oid,
                                    uint64_t clientid, std::string owner,
                                    std::vector<std::byte> fh) {
  FileKey key{fsid, oid};
  int64_t expires = now_coarse() + cfg_.lease_seconds;
  std::lock_guard lock(lock_waiters_mu_);
  auto& waiters = lock_waiters_[key];
  for (auto& w : waiters) {
    if (w.clientid == clientid && w.owner == owner) {
      w.expires = expires;  // re-armed by the retry
      return;
    }
  }
  if (waiters.size() >= 16) return;  // bounded, best effort
  waiters.push_back({clientid, std::move(owner), std::move(fh), expires});
}

void StateMgr::notify_lock_waiters(const FileKey& key) {
  std::vector<LockWaiter> waiters;
  {
    std::lock_guard lock(lock_waiters_mu_);
    auto it = lock_waiters_.find(key);
    if (it == lock_waiters_.end()) return;
    waiters = std::move(it->second);
    lock_waiters_.erase(it);
  }
  int64_t now = now_coarse();
  for (auto& w : waiters) {
    if (w.expires < now) continue;  // the waiter gave up a lease ago
    rt::spawn(notify_task(w.clientid, std::move(w.owner), std::move(w.fh)),
              rt::current_reactor());
  }
}

rt::Task<void> StateMgr::notify_task(uint64_t clientid, std::string owner,
                                     std::vector<std::byte> fh) {
  auto client = co_await find_client(clientid);
  if (!client || client->expired.load(std::memory_order_relaxed)) co_return;
  struct Args {
    const std::vector<std::byte>* fh;
    uint64_t clientid;
    const std::string* owner;
  } args{&fh, clientid, &owner};
  bool ok = co_await send_cb(
      client,
      [](const nfsv4::cb::Target& t, const void* p) {
        auto* a = static_cast<const Args*>(p);
        return nfsv4::cb::build_cb_notify_lock(
            t, *a->fh, a->clientid,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(a->owner->data()),
                a->owner->size()));
      },
      &args);
  if (ok) cb_lock_notifies_.fetch_add(1, std::memory_order_relaxed);
}

rt::Task<StateMgr::IoCheck> StateMgr::check_io(const Stateid& sid, uint64_t clientid,
                                               uint32_t fsid, const backend::ObjId& oid,
                                               uint32_t need) {
  IoCheck out;
  if (sid.is_special()) {
    out.special = true;
    if (need == kShareWrite && in_grace()) {  // stateless writes wait for reclaim
      out.status = as_u32(Status::kGrace);
      co_return out;
    }
    if (sid.is_all_one()) co_return out;  // READ bypass: ignores share deny
    // Anonymous stateid IO is subject to share reservations (RFC 8881 §9.1.2), and an
    // anonymous WRITE invalidates read delegations like any other mutation (§5.2).
    FileKey key{fsid, oid};
    std::vector<StateRef> deleg_recalls;
    {
      FileShard& shard = file_shard(key);
      auto lock = co_await shard.mu.lock();
      auto it = shard.table.find(key);
      if (it != shard.table.end()) {
        for (const auto& rec : it->second.opens) {
          if (rec->closed || rec->client->expired.load(std::memory_order_relaxed))
            continue;
          if (rec->deny & need) {
            out.status = as_u32(Status::kLocked);
            break;
          }
        }
        if (out.status == 0 && need == kShareWrite) {
          for (const auto& d : it->second.delegs)
            if (!d->closed && !d->client->expired.load(std::memory_order_relaxed))
              deleg_recalls.push_back(d);
        }
      }
    }
    if (!deleg_recalls.empty()) {
      for (const auto& d : deleg_recalls) start_recall(d);
      out.status = as_u32(Status::kDelay);
    }
    co_return out;
  }
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch)) {
    out.status = as_u32(Status::kStaleStateid);  // pre-restart stateid: no table walk
    co_return out;
  }
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) {
      out.status = as_u32(Status::kBadStateid);
      co_return out;
    }
    out.rec = it->second;
    out.bopen = it->second->bopen;
  }
  const StateRec& rec = *out.rec;
  const StateRec& open = rec.type == StateType::kLock && rec.parent_open ? *rec.parent_open : rec;
  if (rec.client->expired.load(std::memory_order_relaxed)) {
    out.status = as_u32(Status::kExpired);
  } else if (rec.client->clientid != clientid || rec.fsid != fsid || !(rec.oid == oid)) {
    out.status = as_u32(Status::kBadStateid);
  } else if (sid.seqid != 0 && sid.seqid < rec.seqid) {
    out.status = as_u32(Status::kOldStateid);
  } else if (sid.seqid != 0 && sid.seqid > rec.seqid) {
    out.status = as_u32(Status::kBadStateid);
  } else if ((open.access & need) == 0) {
    out.status = as_u32(Status::kOpenmode);
  }
  if (rec.type == StateType::kLock && out.status == 0) {
    // The parent open's bopen is only stable under ITS shard lock (hashed by the
    // parent's `other`, not the lock stateid's): a concurrent CLOSE moves it out
    // there in unlink_state().  The lock stateid's shard was dropped above, so
    // reacquiring here keeps the one-lock-at-a-time discipline.
    StateShard& shard = state_shard(open.other);
    auto lock = co_await shard.mu.lock();
    out.bopen = open.bopen;
  }
  co_return out;
}

rt::Task<StateMgr::StateLookup> StateMgr::lookup_stateid(const Stateid& sid) {
  StateLookup out;
  if (sid.is_special()) {
    out.special = true;
    co_return out;
  }
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch)) {
    out.status = as_u32(Status::kStaleStateid);
    co_return out;
  }
  StateShard& shard = state_shard(sid.other);
  auto lock = co_await shard.mu.lock();
  auto it = shard.table.find(sid.other);
  if (it == shard.table.end()) {
    out.status = as_u32(Status::kBadStateid);
  } else {
    const StateRec& rec = *it->second;
    out.rec = OpenRec{rec.client->clientid, rec.fsid, rec.oid, rec.seqid.load(),
                      rec.access.load(), rec.deny.load()};
  }
  co_return out;
}

rt::Task<uint32_t> StateMgr::close_state(const Stateid& sid, uint64_t clientid,
                                         Stateid* out) {
  if (sid.is_special()) co_return as_u32(Status::kBadStateid);
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  if (clientid != 0 && rec->client->clientid != clientid)
    co_return as_u32(Status::kBadStateid);
  // seqid discipline (RFC 8881 §8.2.2): zero means "current", older is OLD_STATEID,
  // ahead of the server is BAD_STATEID.
  if (clientid != 0 && sid.seqid != 0) {
    if (sid.seqid < rec->seqid) co_return as_u32(Status::kOldStateid);
    if (sid.seqid > rec->seqid) co_return as_u32(Status::kBadStateid);
  }
  auto released = co_await unlink_state(rec, true);
  if (out) {
    out->other = sid.other;
    out->seqid = rec->seqid + 1;
  }
  released.reset();  // backend handle: after the last lock (07 §6.1)
  co_return 0;
}

rt::Task<uint32_t> StateMgr::close_state(const Stateid& sid) {
  co_return co_await close_state(sid, 0, nullptr);
}

rt::Task<uint32_t> StateMgr::open_downgrade(const Stateid& sid, uint64_t clientid,
                                            uint32_t access, uint32_t deny,
                                            Stateid* out) {
  if (sid.is_special()) co_return as_u32(Status::kBadStateid);
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  if (rec->client->clientid != clientid || rec->type != StateType::kOpen)
    co_return as_u32(Status::kBadStateid);
  if (sid.seqid != 0 && sid.seqid < rec->seqid) co_return as_u32(Status::kOldStateid);
  if (sid.seqid != 0 && sid.seqid > rec->seqid) co_return as_u32(Status::kBadStateid);
  // RFC 8881 §18.18: the new modes must be a subset of the current ones and access
  // cannot drop to nothing (that is what CLOSE is for).
  if (access == 0 || (access & ~rec->access) != 0 || (deny & ~rec->deny) != 0)
    co_return as_u32(Status::kInval);
  {
    FileShard& shard = file_shard(FileKey{rec->fsid, rec->oid});
    auto lock = co_await shard.mu.lock();  // serializes with merges on this file
    rec->access = access;
    rec->deny = deny;
    rec->seqid += 1;
  }
  if (out) {
    out->other = sid.other;
    out->seqid = rec->seqid;
  }
  co_return 0;
}

rt::Task<uint32_t> StateMgr::free_stateid(const Stateid& sid) {
  // FREE_STATEID (RFC 8881 §18.38): only a lock stateid whose ranges are all released
  // may be freed; open and delegation stateids answer LOCKS_HELD (they are released by
  // CLOSE / DELEGRETURN instead).
  if (sid.is_special()) co_return as_u32(Status::kBadStateid);
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  // Open stateids are released by CLOSE; a lock stateid can be freed once its ranges
  // are gone (RFC 8881 §18.38.3), otherwise LOCKS_HELD.
  if (rec->type != StateType::kLock) co_return as_u32(Status::kLocksHeld);
  if (locks_.count_owner(FileKey{rec->fsid, rec->oid}, rec->lowner) > 0)
    co_return as_u32(Status::kLocksHeld);
  (void)co_await unlink_state(rec, true);
  co_return 0;
}

// ---- byte-range locks ------------------------------------------------------

backend::LockOwnerId StateMgr::make_lowner(uint64_t clientid, std::string_view owner) {
  backend::LockOwnerId id;
  id.len = 20;
  std::memcpy(id.bytes.data(), &clientid, 8);
  uint64_t h = fnv64(owner);
  std::memcpy(id.bytes.data() + 8, &h, 8);
  uint32_t len = static_cast<uint32_t>(owner.size());
  std::memcpy(id.bytes.data() + 16, &len, 4);
  return id;
}

void StateMgr::remember_lock_owner(const backend::LockOwnerId& id,
                                   std::shared_ptr<ClientRec> client, std::string owner) {
  std::lock_guard g(lock_owner_mu_);
  lock_owners_[std::string(reinterpret_cast<const char*>(id.bytes.data()), id.len)] =
      LockOwnerRec{std::move(client), std::move(owner)};
}

std::optional<StateMgr::LockOwnerRec> StateMgr::find_lock_owner(
    const backend::LockOwnerId& id) {
  std::lock_guard g(lock_owner_mu_);
  auto it = lock_owners_.find(
      std::string(reinterpret_cast<const char*>(id.bytes.data()), id.len));
  if (it == lock_owners_.end()) return std::nullopt;
  return it->second;
}

rt::Task<StateMgr::LockResult> StateMgr::lock(LockArgs args) {
  LockResult out;
  auto client = co_await find_client(args.clientid);
  if (!client || !client->confirmed) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  if (client->expired.load(std::memory_order_relaxed)) {
    out.status = as_u32(Status::kExpired);
    co_return out;
  }
  bool reclaim_done;
  size_t held_states;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    reclaim_done = client->reclaim_complete;
    held_states = client->states.size();
  }
  if (args.reclaim) {  // same grace gate as OPEN(CLAIM_PREVIOUS)
    if (!in_grace() || reclaim_done) {
      out.status = as_u32(Status::kNoGrace);
      co_return out;
    }
    if (!in_stable_list(client->owner_id)) {
      out.status = as_u32(Status::kReclaimBad);
      co_return out;
    }
  } else if (in_grace() || !reclaim_done) {
    out.status = as_u32(Status::kGrace);
    co_return out;
  }

  // Resolve the lock-owner and its anchoring open state.
  StateRef open_rec, lock_rec;
  std::string owner_bytes;
  if (args.new_owner) {
    if (args.open_stateid.is_special() ||
        epoch_of(args.open_stateid.other) != static_cast<uint32_t>(cfg_.boot_epoch)) {
      out.status = args.open_stateid.is_special() ? as_u32(Status::kBadStateid)
                                                  : as_u32(Status::kStaleStateid);
      co_return out;
    }
    {
      StateShard& shard = state_shard(args.open_stateid.other);
      auto lock = co_await shard.mu.lock();
      auto it = shard.table.find(args.open_stateid.other);
      if (it != shard.table.end()) open_rec = it->second;
    }
    if (!open_rec || open_rec->type != StateType::kOpen || open_rec->client != client ||
        open_rec->fsid != args.fsid || !(open_rec->oid == args.oid)) {
      out.status = as_u32(Status::kBadStateid);
      co_return out;
    }
    uint32_t s = args.open_stateid.seqid;
    if (s != 0 && s < open_rec->seqid) {
      out.status = as_u32(Status::kOldStateid);
      co_return out;
    }
    if (s != 0 && s > open_rec->seqid) {
      out.status = as_u32(Status::kBadStateid);
      co_return out;
    }
    owner_bytes = args.owner;
  } else {
    if (args.lock_stateid.is_special() ||
        epoch_of(args.lock_stateid.other) != static_cast<uint32_t>(cfg_.boot_epoch)) {
      out.status = args.lock_stateid.is_special() ? as_u32(Status::kBadStateid)
                                                  : as_u32(Status::kStaleStateid);
      co_return out;
    }
    {
      StateShard& shard = state_shard(args.lock_stateid.other);
      auto lock = co_await shard.mu.lock();
      auto it = shard.table.find(args.lock_stateid.other);
      if (it != shard.table.end()) lock_rec = it->second;
    }
    if (!lock_rec || lock_rec->type != StateType::kLock || lock_rec->client != client ||
        lock_rec->fsid != args.fsid || !(lock_rec->oid == args.oid)) {
      out.status = as_u32(Status::kBadStateid);
      co_return out;
    }
    uint32_t s = args.lock_stateid.seqid;
    if (s != 0 && s < lock_rec->seqid) {
      out.status = as_u32(Status::kOldStateid);
      co_return out;
    }
    if (s != 0 && s > lock_rec->seqid) {
      out.status = as_u32(Status::kBadStateid);
      co_return out;
    }
    open_rec = lock_rec->parent_open;
    owner_bytes = lock_rec->owner;
  }
  // The lock type must be covered by the open's access mode (RFC 8881 §18.10.3).
  uint32_t need = args.exclusive ? kShareWrite : kShareRead;
  if (!open_rec || open_rec->closed || (open_rec->access & need) == 0) {
    out.status = as_u32(Status::kOpenmode);
    co_return out;
  }
  backend::LockOwnerId lowner = make_lowner(args.clientid, owner_bytes);
  FileKey key{args.fsid, args.oid};
  backend::LockRange range{args.offset, args.length};

  // Resource caps (plan doc 10 §1.5): a new lock stateid counts against the client's
  // state cap, and per-(owner,file) segment fragmentation is bounded so alternating
  // ranges cannot grow the interval list without limit.
  if (args.new_owner && cfg_.max_states_per_client != 0 &&
      held_states >= cfg_.max_states_per_client) {
    out.status = as_u32(Status::kResource);
    co_return out;
  }
  if (cfg_.max_lock_segments_per_owner != 0 &&
      locks_.count_owner(key, lowner) >= cfg_.max_lock_segments_per_owner) {
    out.status = as_u32(Status::kResource);
    co_return out;
  }

  // The owner's coverage before this grant: restored if the native push refuses.
  backend::LockMgr* native = native_lock_mgr(args.fsid);
  std::vector<LockSeg> before;
  if (native)
    for (const auto& seg : locks_.segments(key))
      if (same_owner(seg.owner, lowner)) before.push_back(seg);

  // Arbitrate; a conflict held by a courtesy client reclaims it first (07 §7.4).
  for (int attempt = 0;; ++attempt) {
    auto conflict = locks_.lock(key, lowner, range, args.exclusive);
    if (!conflict) break;
    auto holder = find_lock_owner(conflict->owner);
    if (holder && holder->client->courtesy.load(std::memory_order_relaxed) && attempt < 8) {
      (void)co_await expire_client_impl(holder->client->clientid, kReasonConflict);
      continue;
    }
    if (holder && holder->client->expired.load(std::memory_order_relaxed) && attempt < 8) {
      co_await rt::sleep_for(std::chrono::milliseconds(1));  // reclaim chain in flight
      continue;
    }
    lock_denied_.fetch_add(1, std::memory_order_relaxed);
    out.status = as_u32(Status::kDenied);
    out.denied.offset = conflict->range.offset;
    out.denied.length = conflict->range.length;
    out.denied.exclusive = conflict->exclusive;
    if (holder) {
      out.denied.clientid = holder->client->clientid;
      out.denied.owner = holder->owner;
    }
    co_return out;
  }
  // Native push (design 05 §5.8, plan doc 10 §5.3): the backend's lock manager sees
  // the same grant, so a gateway sharing the storage conflicts with this one.  A
  // refusal undoes the gateway grant (previous coverage restored) and answers like a
  // local conflict; a backend error answers DELAY/SERVERFAULT with nothing granted.
  if (native) {
    auto pushed = co_await push_native_lock(*native, args.fsid, args.oid, lowner, range,
                                            args.exclusive);
    if (pushed.status != 0) {
      locks_.release_owner(key, lowner);
      for (const auto& seg : before)
        (void)locks_.lock(key, lowner, GatewayLockMgr::to_range(seg.start, seg.end),
                          seg.exclusive);
      // A reclaim refused by the storage inside grace (design 09 §9.7, plan 10 B2) is
      // most likely the failed gateway's own lock lingering until the storage times
      // its session out: DELAY keeps the client retrying rather than dropping the
      // lock.  Once grace is over a refusal is a real third-party holder: DENIED.
      if (pushed.status == as_u32(Status::kDenied) && args.reclaim && in_grace()) {
        native_lock_reclaim_delays_.fetch_add(1, std::memory_order_relaxed);
        pushed.status = as_u32(Status::kDelay);
        pushed.denied = {};
      }
      co_return pushed;
    }
  }
  remember_lock_owner(lowner, client, owner_bytes);

  // Granted: reuse the (lock-owner, file) stateid or mint one.
  bool created = false;
  {
    FileShard& shard = file_shard(key);
    auto lock = co_await shard.mu.lock();
    auto& file = shard.table[key];
    if (!lock_rec) {
      for (const auto& l : file.locks)
        if (!l->closed && same_owner(l->lowner, lowner)) lock_rec = l;
    }
    if (!lock_rec) {
      lock_rec = std::make_shared<StateRec>();
      lock_rec->type = StateType::kLock;
      lock_rec->other = new_other(StateType::kLock);
      lock_rec->client = client;
      lock_rec->fsid = args.fsid;
      lock_rec->oid = args.oid;
      lock_rec->owner = owner_bytes;
      lock_rec->lowner = lowner;
      lock_rec->parent_open = open_rec;
      lock_rec->seqid = 0;  // bumped below
      if (file.opens.empty() && file.locks.empty())
        file_count_.fetch_add(1, std::memory_order_relaxed);
      file.locks.push_back(lock_rec);
      created = true;
    }
    lock_rec->seqid += 1;
  }
  if (created) {
    {
      StateShard& shard = state_shard(lock_rec->other);
      auto lock = co_await shard.mu.lock();
      shard.table[lock_rec->other] = lock_rec;
      lock_count_.fetch_add(1, std::memory_order_relaxed);
    }
    bool dead;
    {
      ClientShard& shard = owner_shard(client->owner_id);
      auto lock = co_await shard.mu.lock();
      dead = client->expired.load(std::memory_order_relaxed);
      if (!dead) client->states.insert(lock_rec->other);
    }
    if (dead) {
      (void)co_await unlink_state(lock_rec, false);
      out.status = as_u32(Status::kExpired);
      co_return out;
    }
  }
  out.stateid.seqid = lock_rec->seqid;
  out.stateid.other = lock_rec->other;
  co_return out;
}

rt::Task<StateMgr::LockResult> StateMgr::lockt(uint64_t clientid, uint32_t fsid,
                                               const backend::ObjId& oid, std::string owner,
                                               bool exclusive, uint64_t offset,
                                               uint64_t length) {
  LockResult out;
  auto client = co_await find_client(clientid);
  if (!client || !client->confirmed) {
    out.status = as_u32(Status::kStaleClientid);
    co_return out;
  }
  backend::LockOwnerId lowner = make_lowner(clientid, owner);
  FileKey key{fsid, oid};
  for (int attempt = 0;; ++attempt) {
    auto conflict = locks_.test(key, &lowner, {offset, length}, exclusive);
    if (!conflict) break;
    auto holder = find_lock_owner(conflict->owner);
    if (holder && holder->client->courtesy.load(std::memory_order_relaxed) && attempt < 8) {
      (void)co_await expire_client_impl(holder->client->clientid, kReasonConflict);
      continue;
    }
    out.status = as_u32(Status::kDenied);
    out.denied.offset = conflict->range.offset;
    out.denied.length = conflict->range.length;
    out.denied.exclusive = conflict->exclusive;
    if (holder) {
      out.denied.clientid = holder->client->clientid;
      out.denied.owner = holder->owner;
    }
    co_return out;
  }
  // No local conflict: ask the backend about other gateways (F_GETLK-style probe).
  // The probe cannot tell the asker's own locks from foreign ones, so it is skipped
  // while this owner already covers part of the range — the gateway table has
  // answered for that case.
  backend::LockMgr* native = native_lock_mgr(fsid);
  if (!native) co_return out;
  uint64_t end = GatewayLockMgr::range_end({offset, length});
  for (const auto& seg : locks_.segments(key))
    if (same_owner(seg.owner, lowner) && seg.start < end && offset < seg.end) co_return out;
  auto obj = co_await native_lock_object(fsid, oid);
  if (!obj) {
    out.status = native_lock_status(obj.error());
    co_return out;
  }
  auto probe = co_await native->test(**obj, {offset, length}, exclusive);
  if (!probe) {
    native_lock_errors_.fetch_add(1, std::memory_order_relaxed);
    out.status = native_lock_status(probe.error());
    co_return out;
  }
  if (*probe) {
    out.status = as_u32(Status::kDenied);
    out.denied.offset = (*probe)->range.offset;
    out.denied.length = (*probe)->range.length;
    out.denied.exclusive = (*probe)->exclusive;
    if ((*probe)->owner.len > 0)
      if (auto holder = find_lock_owner((*probe)->owner)) {
        out.denied.clientid = holder->client->clientid;
        out.denied.owner = holder->owner;
      }
  }
  co_return out;
}

backend::LockMgr* StateMgr::native_lock_mgr(uint32_t fsid) const {
  return cfg_.native_locks.manager ? cfg_.native_locks.manager(fsid) : nullptr;
}

rt::Task<Result<backend::ObjPtr>> StateMgr::native_lock_object(uint32_t fsid,
                                                              const backend::ObjId& oid) {
  if (!cfg_.native_locks.resolve) co_return Err(errno_from(ENODEV));
  co_return co_await cfg_.native_locks.resolve(fsid, oid);
}

uint32_t StateMgr::native_lock_status(Errno error) {
  if (error == Errno::kJukebox || error == errno_from(ENOTCONN) ||
      error == errno_from(ETIMEDOUT) || error == errno_from(EAGAIN))
    return as_u32(Status::kDelay);
  if (error == errno_from(ESTALE) || error == errno_from(ENOENT))
    return as_u32(Status::kStale);
  return as_u32(Status::kServerfault);
}

rt::Task<StateMgr::LockResult> StateMgr::push_native_lock(
    backend::LockMgr& native, uint32_t fsid, const backend::ObjId& oid,
    const backend::LockOwnerId& lowner, backend::LockRange range, bool exclusive) {
  LockResult out;
  auto obj = co_await native_lock_object(fsid, oid);
  if (!obj) {
    native_lock_errors_.fetch_add(1, std::memory_order_relaxed);
    out.status = native_lock_status(obj.error());
    co_return out;
  }
  auto granted = co_await native.lock(**obj, lowner, range, exclusive, false);
  if (granted) co_return out;
  if (granted.error() != errno_from(EAGAIN)) {
    native_lock_errors_.fetch_add(1, std::memory_order_relaxed);
    out.status = native_lock_status(granted.error());
    co_return out;
  }
  // Refused by the storage: someone (typically another gateway) holds it.
  native_lock_denied_.fetch_add(1, std::memory_order_relaxed);
  lock_denied_.fetch_add(1, std::memory_order_relaxed);
  out.status = as_u32(Status::kDenied);
  out.denied.offset = range.offset;
  out.denied.length = range.length;
  out.denied.exclusive = exclusive;
  auto conflict = co_await native.test(**obj, range, exclusive);
  if (conflict && *conflict) {
    out.denied.offset = (*conflict)->range.offset;
    out.denied.length = (*conflict)->range.length;
    out.denied.exclusive = (*conflict)->exclusive;
    if ((*conflict)->owner.len > 0)
      if (auto holder = find_lock_owner((*conflict)->owner)) {
        out.denied.clientid = holder->client->clientid;
        out.denied.owner = holder->owner;
      }
  }
  co_return out;
}

rt::Task<uint32_t> StateMgr::locku(const Stateid& sid, uint64_t clientid, uint64_t offset,
                                   uint64_t length, Stateid* out) {
  if (sid.is_special()) co_return as_u32(Status::kBadStateid);
  if (epoch_of(sid.other) != static_cast<uint32_t>(cfg_.boot_epoch))
    co_return as_u32(Status::kStaleStateid);
  StateRef rec;
  {
    StateShard& shard = state_shard(sid.other);
    auto lock = co_await shard.mu.lock();
    auto it = shard.table.find(sid.other);
    if (it == shard.table.end()) co_return as_u32(Status::kBadStateid);
    rec = it->second;
  }
  if (rec->type != StateType::kLock || rec->client->clientid != clientid)
    co_return as_u32(Status::kBadStateid);
  if (sid.seqid != 0 && sid.seqid < rec->seqid) co_return as_u32(Status::kOldStateid);
  if (sid.seqid != 0 && sid.seqid > rec->seqid) co_return as_u32(Status::kBadStateid);
  locks_.unlock(FileKey{rec->fsid, rec->oid}, rec->lowner, {offset, length});
  if (backend::LockMgr* native = native_lock_mgr(rec->fsid)) {
    // Mirror the release into the storage.  A failure leaves the native lock until
    // the owner's descriptor is closed (CLOSE / expiry); the gateway view is already
    // unlocked, so the client is not told to retry a range it no longer holds.
    auto obj = co_await native_lock_object(rec->fsid, rec->oid);
    Result<void> released = obj ? co_await native->unlock(**obj, rec->lowner, {offset, length})
                                : Result<void>(Err(obj.error()));
    if (!released) {
      native_lock_errors_.fetch_add(1, std::memory_order_relaxed);
      LNFS_WARN("native unlock failed (fsid {}): {}", rec->fsid, errno_name(released.error()));
    }
  }
  {
    FileShard& shard = file_shard(FileKey{rec->fsid, rec->oid});
    auto lock = co_await shard.mu.lock();  // serializes seqid bumps with LOCK
    rec->seqid += 1;
  }
  if (out) {
    out->other = sid.other;
    out->seqid = rec->seqid;
  }
  // The released range may unblock waiters (plan doc 10 §5.2): CB_NOTIFY_LOCK them.
  notify_lock_waiters(FileKey{rec->fsid, rec->oid});
  co_return 0;
}

// ---- leases ----------------------------------------------------------------

rt::Task<void> StateMgr::scan_leases() {
  // Expiry heap (plan doc 10 §2.6): pop due entries and re-check them against the
  // live record instead of walking every client every second.  Renewals never touch
  // the heap (the SEQUENCE fast path stays a lone atomic store); a renewed client's
  // stale entry simply re-arms at its fresh expiry when it pops.
  int64_t now = now_coarse();
  int64_t courtesy_window = static_cast<int64_t>(cfg_.lease_seconds) *
                            std::max<uint32_t>(1, cfg_.courtesy_multiplier);
  std::vector<std::shared_ptr<ClientRec>> due;
  {
    std::lock_guard heap(lease_heap_mu_);
    std::lock_guard idx(client_idx_mu_);
    while (!lease_heap_.empty() && lease_heap_.top().first <= now) {
      auto [when, clientid] = lease_heap_.top();
      lease_heap_.pop();
      auto it = client_idx_.find(clientid);
      if (it == client_idx_.end()) continue;              // client already gone
      if (it->second->lease_heap_deadline != when) continue;  // superseded duplicate
      it->second->lease_heap_deadline = -1;
      due.push_back(it->second);
    }
  }
  std::vector<uint64_t> timed_out;
  for (const auto& client : due) {
    if (client->expired.load(std::memory_order_relaxed)) continue;
    if (client->courtesy.load(std::memory_order_relaxed)) {
      if (now - client->courtesy_since >= courtesy_window) {
        timed_out.push_back(client->clientid);  // reclaim drops the record
      } else {
        arm_lease_check(*client, client->courtesy_since + courtesy_window);
      }
      continue;
    }
    int64_t expiry = client->lease_expiry.load(std::memory_order_relaxed);
    if (expiry > now) {  // renewed since the entry was armed
      arm_lease_check(*client, expiry);
      continue;
    }
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(client->clientid);
    if (it == shard.by_id.end() || it->second != client) continue;  // replaced/gone
    ClientRec& c = *client;
    // Re-check under the lock: a SEQUENCE may have renewed meanwhile.
    expiry = c.lease_expiry.load(std::memory_order_relaxed);
    if (c.expired.load(std::memory_order_relaxed)) continue;
    if (expiry > now || c.courtesy.load(std::memory_order_relaxed)) {
      arm_lease_check(c, std::max(expiry, now + 1));
      continue;
    }
    if (!c.confirmed || (c.sessions.empty() && c.states.empty())) {
      // Nothing to protect: an unconfirmed or idle record simply lapses.
      unpersist_client(c);
      auto slot_it = shard.by_owner.find(c.owner_id);
      if (slot_it != shard.by_owner.end()) {
        if (slot_it->second.confirmed == client) slot_it->second.confirmed.reset();
        if (slot_it->second.unconfirmed == client) slot_it->second.unconfirmed.reset();
        if (!slot_it->second.confirmed && !slot_it->second.unconfirmed)
          shard.by_owner.erase(slot_it);
      }
      shard.by_id.erase(it);
      unindex_client(c.clientid);
      client_count_.fetch_sub(1, std::memory_order_relaxed);
      lease_expirations_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    c.courtesy.store(true, std::memory_order_relaxed);
    c.courtesy_since = now;
    courtesy_count_.fetch_add(1, std::memory_order_relaxed);
    lease_expirations_.fetch_add(1, std::memory_order_relaxed);
    LNFS_INFO("client {:#x} ({} sessions, {} states) lease expired: courtesy",
              c.clientid, c.sessions.size(), c.states.size());
    arm_lease_check(c, now + courtesy_window);
  }
  for (uint64_t id : timed_out) (void)co_await expire_client_impl(id, kReasonTimeout);
  // Recalled delegations past their deadline are revoked here (plan doc 10 §5.2).
  co_await sweep_recalls();
}

rt::Task<void> StateMgr::run_lease_scanner(std::atomic<bool>* stop) {
  while (!stop->load(std::memory_order_relaxed)) {
    co_await rt::sleep_for(std::chrono::seconds(1));
    if (stop->load(std::memory_order_relaxed)) break;
    co_await scan_leases();
  }
}

rt::Task<uint32_t> StateMgr::expire_client(uint64_t clientid) {
  co_return co_await expire_client_impl(clientid, kReasonForced);
}

rt::Task<uint32_t> StateMgr::expire_client_impl(uint64_t clientid, int reason) {
  std::shared_ptr<ClientRec> client = find_client_sync(clientid);
  if (!client) co_return as_u32(Status::kStaleClientid);
  std::vector<StateOther> states;
  std::vector<SessionId> sessions;
  {
    ClientShard& shard = owner_shard(client->owner_id);
    auto lock = co_await shard.mu.lock();
    auto it = shard.by_id.find(clientid);
    if (it == shard.by_id.end() || it->second != client)
      co_return as_u32(Status::kStaleClientid);  // raced with removal/replacement
    if (client->expired.exchange(true, std::memory_order_relaxed)) co_return 0;
    if (client->courtesy.exchange(false, std::memory_order_relaxed))
      courtesy_count_.fetch_sub(1, std::memory_order_relaxed);
    states.assign(client->states.begin(), client->states.end());
    client->states.clear();
    sessions = std::move(client->sessions);
    client->sessions.clear();
    auto slot_it = shard.by_owner.find(client->owner_id);
    if (slot_it != shard.by_owner.end()) {
      if (slot_it->second.confirmed == client) slot_it->second.confirmed.reset();
      if (slot_it->second.unconfirmed == client) slot_it->second.unconfirmed.reset();
      if (!slot_it->second.confirmed && !slot_it->second.unconfirmed)
        shard.by_owner.erase(slot_it);
    }
    shard.by_id.erase(it);
    unindex_client(clientid);
    client_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  // Reclaim chain (07 §7.4): StateRec → OpenPtr → files_ back-reference → sessions →
  // stable list.  Every step takes exactly one shard lock.
  std::vector<backend::OpenPtr> released;
  for (const auto& other : states) {
    StateRef rec;
    {
      StateShard& shard = state_shard(other);
      auto lock = co_await shard.mu.lock();
      auto it = shard.table.find(other);
      if (it != shard.table.end()) rec = it->second;
    }
    if (rec) released.push_back(co_await unlink_state(rec, false));
  }
  for (const auto& id : sessions) {
    SessionShard& shard = session_shard(id);
    auto lock = co_await shard.mu.lock();
    auto sit = shard.table.find(id);
    if (sit != shard.table.end()) {
      auto dead = std::move(sit->second);
      shard.table.erase(sit);
      session_count_.fetch_sub(1, std::memory_order_relaxed);
      wake_all_slots(*dead);  // before the last reference drops
    }
  }
  unpersist_client(*client);
  {
    // The lock-owner resolution table only ever grew (plan doc 10 §1.5): drop the
    // reclaimed client's entries so it is bounded by live clients' owners.
    std::lock_guard g(lock_owner_mu_);
    std::erase_if(lock_owners_, [&](const auto& kv) {
      return kv.second.client->clientid == clientid;
    });
  }
  switch (reason) {
    case kReasonConflict: reclaim_conflict_.fetch_add(1, std::memory_order_relaxed); break;
    case kReasonTimeout: reclaim_timeout_.fetch_add(1, std::memory_order_relaxed); break;
    case kReasonForced: reclaim_forced_.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
  }
  LNFS_INFO("client {:#x} reclaimed ({}): {} states, {} sessions released",
            clientid,
            reason == kReasonConflict ? "courtesy conflict"
            : reason == kReasonTimeout ? "courtesy timeout"
            : reason == kReasonReboot ? "client reboot"
                                      : "forced",
            states.size(), sessions.size());
  released.clear();  // backend handles drop here, outside every shard lock
  co_return 0;
}

// ---- observation -----------------------------------------------------------

StateMgr::Stats StateMgr::stats() const {
  auto nonneg = [](const std::atomic<int64_t>& v) {
    return static_cast<size_t>(std::max<int64_t>(0, v.load(std::memory_order_relaxed)));
  };
  Stats out;
  out.clients = nonneg(client_count_);
  out.sessions = nonneg(session_count_);
  out.opens = nonneg(open_count_);
  out.files = nonneg(file_count_);
  out.courtesy = nonneg(courtesy_count_);
  out.seq_new = seq_new_.load(std::memory_order_relaxed);
  out.seq_replay = seq_replay_.load(std::memory_order_relaxed);
  out.seq_misordered = seq_misordered_.load(std::memory_order_relaxed);
  out.seq_waits = seq_waits_.load(std::memory_order_relaxed);
  out.lease_expirations = lease_expirations_.load(std::memory_order_relaxed);
  out.reclaim_conflict = reclaim_conflict_.load(std::memory_order_relaxed);
  out.reclaim_timeout = reclaim_timeout_.load(std::memory_order_relaxed);
  out.reclaim_forced = reclaim_forced_.load(std::memory_order_relaxed);
  out.share_denied = share_denied_.load(std::memory_order_relaxed);
  out.open_merges = open_merges_.load(std::memory_order_relaxed);
  out.lock_states = nonneg(lock_count_);
  out.lock_segments = locks_.total_segments();
  {
    std::lock_guard g(const_cast<std::mutex&>(lock_owner_mu_));
    out.lock_owners = lock_owners_.size();
  }
  out.lock_denied = lock_denied_.load(std::memory_order_relaxed);
  out.native_lock_denied = native_lock_denied_.load(std::memory_order_relaxed);
  out.native_lock_errors = native_lock_errors_.load(std::memory_order_relaxed);
  out.native_lock_reclaim_delays = native_lock_reclaim_delays_.load(std::memory_order_relaxed);
  out.delegs = nonneg(deleg_count_);
  out.deleg_grants = deleg_grants_.load(std::memory_order_relaxed);
  out.deleg_recalls = deleg_recalls_.load(std::memory_order_relaxed);
  out.deleg_returns = deleg_returns_.load(std::memory_order_relaxed);
  out.deleg_revokes = deleg_revokes_.load(std::memory_order_relaxed);
  out.cb_lock_notifies = cb_lock_notifies_.load(std::memory_order_relaxed);
  out.grace = in_grace();
  out.grace_remaining = grace_remaining_seconds();
  return out;
}

rt::Task<std::string> StateMgr::dump() {
  std::string out;
  int64_t now = now_coarse();
  out += std::format("grace={} remaining={}s boot_epoch={}\n", in_grace() ? 1 : 0,
                     grace_remaining_seconds(), cfg_.boot_epoch);
  for (size_t si = 0; si < shard_count_; ++si) {
    auto& shard = clients_[si];
    auto lock = co_await shard.mu.lock();
    for (const auto& [id, c] : shard.by_id) {
      std::string owner_hex;
      for (unsigned char ch : c->owner_id) owner_hex += std::format("{:02x}", ch);
      out += std::format(
          "client {:#x} owner={} principal={} confirmed={} courtesy={} lease_left={}s "
          "sessions={} states={} reclaim_complete={}\n",
          id, owner_hex, c->principal, c->confirmed ? 1 : 0,
          c->courtesy.load(std::memory_order_relaxed) ? 1 : 0,
          c->lease_expiry.load(std::memory_order_relaxed) - now, c->sessions.size(),
          c->states.size(), c->reclaim_complete ? 1 : 0);
    }
  }
  for (size_t si = 0; si < shard_count_; ++si) {
    auto& shard = sessions_[si];
    auto lock = co_await shard.mu.lock();
    for (const auto& [id, s] : shard.table) {
      size_t cached = 0, in_flight = 0;
      for (uint32_t k = 0; k < s->slot_count; ++k) {
        cached += s->slots[k].cached ? 1 : 0;
        in_flight += s->slots[k].in_flight ? 1 : 0;
      }
      std::string sid_hex;
      for (std::byte b : id) sid_hex += std::format("{:02x}", static_cast<unsigned>(b));
      out += std::format("session {} client={:#x} slots={} cached={} in_flight={} conns={}\n",
                         sid_hex, s->client->clientid, s->slot_count, cached, in_flight,
                         s->bound_conns.size());
    }
  }
  for (size_t si = 0; si < shard_count_; ++si) {
    auto& shard = states_[si];
    auto lock = co_await shard.mu.lock();
    for (const auto& [other, rec] : shard.table) {
      std::string oid_hex;
      for (std::byte b : rec->oid.view())
        oid_hex += std::format("{:02x}", static_cast<unsigned>(b));
      std::string owner_hex;
      for (unsigned char ch : rec->owner) owner_hex += std::format("{:02x}", ch);
      if (rec->type == StateType::kLock) {
        std::string ranges;
        for (const auto& seg : locks_.segments(FileKey{rec->fsid, rec->oid}))
          if (same_owner(seg.owner, rec->lowner))
            ranges += std::format("[{},{}){} ", seg.start,
                                  seg.end == UINT64_MAX ? std::string("EOF")
                                                        : std::to_string(seg.end),
                                  seg.exclusive ? "W" : "R");
        out += std::format(
            "lock {} seqid={} client={:#x} fsid={} oid={} owner={} parent={} ranges={}\n",
            hex_other(other), rec->seqid.load(), rec->client->clientid, rec->fsid, oid_hex,
            owner_hex, rec->parent_open ? hex_other(rec->parent_open->other) : "-", ranges);
        continue;
      }
      out += std::format(
          "open {} seqid={} client={:#x} fsid={} oid={} owner={} access={} deny={}\n",
          hex_other(other), rec->seqid.load(), rec->client->clientid, rec->fsid, oid_hex,
          owner_hex, rec->access.load(), rec->deny.load());
    }
  }
  co_return out;
}

}  // namespace lnfs::state
