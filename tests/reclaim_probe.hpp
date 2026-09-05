#pragma once
// Shared harness for the multi-gateway reclaim-lock fault drills (plan 10 E2): a
// gateway B that has just taken over — in grace, with one listed client that held an
// open + byte-range lock on the dead gateway — driving the client's reclaim LOCK
// through a *real* backend LockMgr.  While the storage still holds the dead gateway's
// lock the push is refused and the state layer answers NFS4ERR_DELAY (plan 10 B2);
// once the storage lets go (a CephFS session reclaim, a Gluster ping-timeout, a Lustre
// obd_timeout — simulated by each backend's fake or a competing OFD lock) the retry
// wins, all inside grace.  The state DELAY machinery itself is unit-tested with a fake
// LockMgr in test_state.cpp; this harness proves the same path end to end over each
// backend's own lock implementation and its stale-lock injection.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "backend/api.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "runtime/runtime.hpp"
#include "state/state_mgr.hpp"

namespace lnfs::test {

template <class T>
inline T run_blocking(rt::Runtime& runtime, rt::Task<T> task) {
  std::mutex mu;
  std::condition_variable cv;
  std::optional<T> out;
  rt::spawn(
      [](rt::Task<T> work, std::mutex* mu, std::condition_variable* cv,
         std::optional<T>* out) -> rt::Task<void> {
        auto v = co_await std::move(work);
        std::lock_guard lock(*mu);
        out->emplace(std::move(v));
        cv->notify_one();
      }(std::move(task), &mu, &cv, &out),
      runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return out.has_value(); });
  return std::move(*out);
}

inline void run_blocking(rt::Runtime& runtime, rt::Task<void> task) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  rt::spawn(
      [](rt::Task<void> work, std::mutex* mu, std::condition_variable* cv,
         bool* done) -> rt::Task<void> {
        co_await std::move(work);
        std::lock_guard lock(*mu);
        *done = true;
        cv->notify_one();
      }(std::move(task), &mu, &cv, &done),
      runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return done; });
}

// A gateway B that has taken over the fence and is in grace with one listed client
// ("dead-gw") that held state on the dead gateway.  `manager`/`resolve` wire B's
// state layer to the backend under test; `fsid`/`oid` name the file to reclaim.
class ReclaimProbe {
 public:
  ReclaimProbe(rt::Runtime& runtime, const std::string& state_dir, uint32_t fsid,
               backend::ObjId oid,
               std::function<backend::LockMgr*(uint32_t)> manager,
               std::function<rt::Task<Result<backend::ObjPtr>>(uint32_t, const backend::ObjId&)>
                   resolve)
      : runtime_(runtime), fsid_(fsid), oid_(oid) {
    // Gateway A: a throwaway instance that lists the client in the stable store.
    { state::StateMgr a(cfg(1, state_dir, {}, {})); run_blocking(runtime_, list_client(a)); }
    // Gateway B: the taking-over instance, armed from A's list, wired to the backend.
    b_ = std::make_unique<state::StateMgr>(cfg(2, state_dir, std::move(manager), std::move(resolve)));
    b_->load_grace_list();
  }

  bool in_grace() const { return b_->in_grace(); }
  state::StateMgr& mgr() { return *b_; }
  uint64_t reclaim_delays() const { return b_->stats().native_lock_reclaim_delays; }
  uint64_t lock_states() const { return b_->stats().lock_states; }

  // OPEN(CLAIM_PREVIOUS): the client re-establishes its open on B (once).
  uint32_t open_reclaim() {
    return run_blocking(runtime_, [](ReclaimProbe* self) -> rt::Task<uint32_t> {
      auto b = co_await self->connect_reclaimer();
      self->clientid_ = b;
      state::StateMgr::OpenArgs oa;
      oa.clientid = b;
      oa.fsid = self->fsid_;
      oa.oid = self->oid_;
      oa.owner = "dead-open";
      oa.access = state::kShareBoth;
      oa.deny = 0;
      oa.reclaim = true;
      auto o = co_await self->b_->open(oa, nullptr);
      self->open_sid_ = o.stateid;
      co_return o.status;
    }(this));
  }

  // One LOCK(reclaim) push of the whole [0,len) range; returns the NFS4 status.
  uint32_t lock_reclaim(uint64_t len = 100) {
    return run_blocking(runtime_, [](ReclaimProbe* self, uint64_t l) -> rt::Task<uint32_t> {
      state::StateMgr::LockArgs la;
      la.clientid = self->clientid_;
      la.fsid = self->fsid_;
      la.oid = self->oid_;
      la.exclusive = true;
      la.offset = 0;
      la.length = l;
      la.new_owner = true;
      la.open_stateid = self->open_sid_;
      la.owner = "dead-lock";
      la.reclaim = true;
      auto r = co_await self->b_->lock(la);
      co_return r.status;
    }(this, len));
  }

  // Retries LOCK(reclaim) up to `attempts` times, `gap` apart, until it leaves DELAY.
  // Returns {delay_count, final_status}.
  struct Outcome { unsigned delays = 0; uint32_t status = 0; };
  Outcome lock_until_settled(unsigned attempts, std::chrono::milliseconds gap) {
    Outcome out;
    for (unsigned i = 0; i < attempts; ++i) {
      out.status = lock_reclaim();
      if (out.status != as_u32(nfsv4::Status::kDelay)) return out;
      ++out.delays;
      std::this_thread::sleep_for(gap);
    }
    return out;
  }

  static uint32_t delay() { return as_u32(nfsv4::Status::kDelay); }

 private:
  static uint32_t as_u32(nfsv4::Status s) { return static_cast<uint32_t>(s); }

  state::StateMgr::Config cfg(
      uint64_t epoch, const std::string& dir,
      std::function<backend::LockMgr*(uint32_t)> manager,
      std::function<rt::Task<Result<backend::ObjPtr>>(uint32_t, const backend::ObjId&)> resolve) {
    state::StateMgr::Config c{.boot_epoch = epoch, .state_dir = dir};
    if (manager) c.native_locks.manager = std::move(manager);
    if (resolve) c.native_locks.resolve = std::move(resolve);
    return c;
  }

  rt::Task<void> list_client(state::StateMgr& a) {
    nfsv4::Verifier verf{};
    verf[0] = std::byte{0x9};
    auto ex = co_await a.exchange_id("dead-gw", verf, "sys/e2/0", false);
    auto cs = co_await a.create_session(ex.clientid, ex.sequenceid, "sys/e2/0", {}, {}, 1);
    co_await a.confirm_create_session(ex.clientid, {std::byte{1}});
    (void)cs;
  }

  rt::Task<uint64_t> connect_reclaimer() {
    if (clientid_) co_return clientid_;
    nfsv4::Verifier verf{};
    verf[0] = std::byte{0x9};  // same co_ownerid + verifier as gateway A: listed
    auto ex = co_await b_->exchange_id("dead-gw", verf, "sys/e2/0", false);
    co_await b_->create_session(ex.clientid, ex.sequenceid, "sys/e2/0", {}, {}, 1);
    co_await b_->confirm_create_session(ex.clientid, {std::byte{1}});
    co_return ex.clientid;  // no RECLAIM_COMPLETE: still reclaiming
  }

  rt::Runtime& runtime_;
  uint32_t fsid_;
  backend::ObjId oid_;
  std::unique_ptr<state::StateMgr> b_;
  uint64_t clientid_ = 0;
  nfsv4::Stateid open_sid_{};
};

}  // namespace lnfs::test
