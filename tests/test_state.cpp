// StateMgr concurrency matrix (development plan §5.3, design 07 §7.2): storms of
// EXCHANGE_ID / CREATE_SESSION / SEQUENCE begin+complete / open+close / DESTROY_* from
// multiple reactors concurrently. Completion of this test is the deadlock-freedom
// assertion (single-shard-at-a-time locking); invariants check table consistency.

#include "mini_test.hpp"

#include <stdlib.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

#include "backend/memory/memory.hpp"
#include "runtime/io.hpp"
#include "runtime/runtime.hpp"
#include "state/state_mgr.hpp"

using namespace lnfs;

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    char tmpl[] = "/tmp/lnfs-state-XXXXXX";
    path = mkdtemp(tmpl);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

TEST(StateMgr, ConcurrentLifecycleMatrixIsDeadlockFree) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 2, .offload_threads = 2});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 3, .state_dir = dir.path});

  constexpr int kClients = 12;
  constexpr int kSeqPerSlot = 25;
  std::atomic<int> done{0};
  std::atomic<int> errors{0};
  std::mutex mu;
  std::condition_variable cv;

  for (int c = 0; c < kClients; ++c) {
    rt::spawn(
        [](state::StateMgr* mgr, int idx, std::atomic<int>* done,
           std::atomic<int>* errors, std::mutex* mu,
           std::condition_variable* cv) -> rt::Task<void> {
          nfsv4::Verifier verf{};
          verf[0] = static_cast<std::byte>(idx);
          auto ex = co_await mgr->exchange_id("client-" + std::to_string(idx), verf,
                                             "sys/test/0", false);
          if (ex.status != 0) errors->fetch_add(1);
          auto cs = co_await mgr->create_session(ex.clientid, ex.sequenceid,
                                                 "sys/test/0", {}, {}, 1);
          if (cs.status != 0) errors->fetch_add(1);
          co_await mgr->confirm_create_session(ex.clientid, {std::byte{1}});
          if (co_await mgr->reclaim_complete(ex.clientid) != 0) errors->fetch_add(1);

          // Interleaved slot traffic on two slots + open/close churn.
          for (uint32_t seq = 1; seq <= kSeqPerSlot; ++seq) {
            for (uint32_t slot = 0; slot < 2; ++slot) {
              auto begin = co_await mgr->sequence_begin(cs.sessionid, slot, seq, 3,
                                                        seq % 2 == 0, 1);
              if (begin.status != 0 || begin.replay) {
                errors->fetch_add(1);
                continue;
              }
              backend::ObjId oid;
              oid.len = 4;
              oid.bytes[0] = static_cast<std::byte>(idx);
              auto sid = co_await mgr->open_read(ex.clientid, 1, oid);
              auto look = co_await mgr->lookup_stateid(sid);
              if (look.status != 0) errors->fetch_add(1);
              if (co_await mgr->close_state(sid) != 0) errors->fetch_add(1);
              co_await mgr->sequence_complete(cs.sessionid, slot, seq, seq % 2 == 0,
                                              {std::byte{7}});
            }
          }
          // Retransmit of the last cached sequence must replay.
          auto replay = co_await mgr->sequence_begin(cs.sessionid, 0, kSeqPerSlot, 3,
                                                     false, 1);
          if (!replay.replay) errors->fetch_add(1);

          if (co_await mgr->destroy_clientid(ex.clientid) !=
              static_cast<uint32_t>(nfsv4::Status::kClientidBusy))
            errors->fetch_add(1);  // sessions still exist: must refuse
          if (co_await mgr->destroy_session(cs.sessionid, 1) != 0) errors->fetch_add(1);
          if (co_await mgr->destroy_clientid(ex.clientid) != 0) errors->fetch_add(1);

          if (done->fetch_add(1) + 1 == kClients) {
            std::lock_guard lock(*mu);
            cv->notify_one();
          }
        }(&mgr, c, &done, &errors, &mu, &cv),
        runtime.reactor(c % 2));
  }

  {
    std::unique_lock lock(mu);
    bool finished = cv.wait_for(lock, std::chrono::seconds(30),
                                [&] { return done.load() == kClients; });
    ASSERT_TRUE(finished);  // a deadlock would hang here
  }
  EXPECT_EQ(errors.load(), 0);
  auto stats = mgr.stats();
  EXPECT_EQ(stats.clients, 0u);
  EXPECT_EQ(stats.sessions, 0u);
  EXPECT_EQ(stats.opens, 0u);
  EXPECT_TRUE(stats.seq_replay >= kClients);
  runtime.stop_and_join();
}

TEST(StateMgr, GraceListPersistsAndEarlyExit) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  uint64_t clientid = 0;
  {
    state::StateMgr first({.boot_epoch = 1, .state_dir = dir.path});
    std::mutex mu;
    std::condition_variable cv;
    bool ok = false;
    rt::spawn(
        [](state::StateMgr* mgr, uint64_t* out, std::mutex* mu,
           std::condition_variable* cv, bool* ok) -> rt::Task<void> {
          auto ex = co_await mgr->exchange_id("grace-client", {}, "sys/t/0", false);
          auto cs = co_await mgr->create_session(ex.clientid, ex.sequenceid,
                                                 "sys/t/0", {}, {}, 1);
          co_await mgr->confirm_create_session(ex.clientid, {std::byte{1}});
          (void)cs;
          *out = ex.clientid;
          {
            std::lock_guard lock(*mu);
            *ok = true;
            cv->notify_one();
          }
        }(&first, &clientid, &mu, &cv, &ok),
        runtime.reactor(0));
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return ok; });
  }

  // "Restart": a fresh manager over the same state_dir arms grace from clients/.
  state::StateMgr second({.boot_epoch = 2, .state_dir = dir.path});
  second.load_grace_list();
  EXPECT_TRUE(second.in_grace());
  EXPECT_TRUE(second.in_stable_list("grace-client"));

  std::mutex mu;
  std::condition_variable cv;
  bool ok = false;
  rt::spawn(
      [](state::StateMgr* mgr, std::mutex* mu, std::condition_variable* cv,
         bool* ok) -> rt::Task<void> {
        auto ex = co_await mgr->exchange_id("grace-client", {}, "sys/t/0", false);
        auto cs = co_await mgr->create_session(ex.clientid, ex.sequenceid,
                                               "sys/t/0", {}, {}, 1);
        (void)cs;
        // All listed clients done -> grace ends early.
        (void)co_await mgr->reclaim_complete(ex.clientid);
        {
          std::lock_guard lock(*mu);
          *ok = true;
          cv->notify_one();
        }
      }(&second, &mu, &cv, &ok),
      runtime.reactor(0));
  {
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return ok; });
  }
  EXPECT_TRUE(!second.in_grace());
  runtime.stop_and_join();
}

// ---- phase 4: open-state table, leases, grace/reclaim (development plan §6) --------

namespace {

// Runs one coroutine on a reactor and blocks the test thread until it finishes.
template <class Fn>
void run_on(rt::Runtime& runtime, Fn fn) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  rt::spawn(
      [](Fn f, std::mutex* mu, std::condition_variable* cv, bool* done) -> rt::Task<void> {
        co_await f();
        std::lock_guard lock(*mu);
        *done = true;
        cv->notify_one();
      }(std::move(fn), &mu, &cv, &done),
      runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return done; });
}

struct Session {
  uint64_t clientid = 0;
  state::SessionId sessionid{};
};

rt::Task<Session> connect(state::StateMgr& mgr, const std::string& owner, uint8_t verf_byte,
                          bool reclaim_complete = true) {
  nfsv4::Verifier verf{};
  verf[0] = static_cast<std::byte>(verf_byte);
  auto ex = co_await mgr.exchange_id(owner, verf, "sys/t/0", false);
  auto cs = co_await mgr.create_session(ex.clientid, ex.sequenceid, "sys/t/0", {}, {}, 1);
  co_await mgr.confirm_create_session(ex.clientid, {std::byte{1}});
  if (reclaim_complete) (void)co_await mgr.reclaim_complete(ex.clientid);
  co_return Session{ex.clientid, cs.sessionid};
}

backend::ObjId oid_of(uint8_t n) {
  backend::ObjId oid;
  oid.len = 4;
  oid.bytes[0] = static_cast<std::byte>(n);
  return oid;
}

state::StateMgr::OpenArgs open_args(uint64_t clientid, uint8_t file, const char* owner,
                                    uint32_t access, uint32_t deny) {
  state::StateMgr::OpenArgs a;
  a.clientid = clientid;
  a.fsid = 1;
  a.oid = oid_of(file);
  a.owner = owner;
  a.access = access;
  a.deny = deny;
  return a;
}

constexpr uint32_t kOk = 0;
uint32_t st4(nfsv4::Status s) { return static_cast<uint32_t>(s); }

}  // namespace

TEST(StateMgr, ShareReservationMergeDowngradeClose) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 5, .state_dir = dir.path});
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "client-a", 1);
    auto b = co_await connect(mgr, "client-b", 2);

    // A: READ, deny WRITE.
    auto o1 = co_await mgr.open(open_args(a.clientid, 1, "oa", state::kShareRead,
                                          state::kShareWrite), nullptr);
    EXPECT_EQ(o1.status, kOk);
    EXPECT_EQ(o1.stateid.seqid, 1u);
    // B: WRITE conflicts with A's deny.
    auto o2 = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(o2.status, st4(nfsv4::Status::kShareDenied));
    // B: READ is fine (A denies only WRITE); B denies nothing.
    auto o3 = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareRead, 0), nullptr);
    EXPECT_EQ(o3.status, kOk);
    // A: same owner re-OPEN for WRITE merges (B's read does not conflict with A's
    // WRITE access; A's deny stays WRITE): seqid 2, same other.
    auto o4 = co_await mgr.open(open_args(a.clientid, 1, "oa", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(o4.status, kOk);
    EXPECT_TRUE(o4.merged);
    EXPECT_TRUE(o4.stateid.other == o1.stateid.other);
    EXPECT_EQ(o4.stateid.seqid, 2u);
    // A: a different owner on the same client is a separate state, subject to A's own
    // deny (deny WRITE vs. access WRITE) -> SHARE_DENIED.
    auto o5 = co_await mgr.open(open_args(a.clientid, 1, "oa2", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(o5.status, st4(nfsv4::Status::kShareDenied));

    // IO checks: WRITE through B's read-only state -> OPENMODE; through A's -> OK;
    // stale seqid -> OLD_STATEID; wrong client -> BAD_STATEID; anonymous WRITE blocked
    // by A's deny -> LOCKED; anonymous READ fine.
    auto c1 = co_await mgr.check_io(o3.stateid, b.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(c1.status, st4(nfsv4::Status::kOpenmode));
    auto c2 = co_await mgr.check_io(o4.stateid, a.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(c2.status, kOk);
    auto c3 = co_await mgr.check_io(o1.stateid, a.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(c3.status, st4(nfsv4::Status::kOldStateid));
    auto c4 = co_await mgr.check_io(o4.stateid, b.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(c4.status, st4(nfsv4::Status::kBadStateid));
    nfsv4::Stateid anon{};
    auto c5 = co_await mgr.check_io(anon, b.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(c5.status, st4(nfsv4::Status::kLocked));
    auto c6 = co_await mgr.check_io(anon, b.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(c6.status, kOk);
    nfsv4::Stateid zero_seq = o4.stateid;
    zero_seq.seqid = 0;  // "don't check version"
    auto c7 = co_await mgr.check_io(zero_seq, a.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(c7.status, kOk);

    // OPEN_DOWNGRADE to READ/deny NONE: seqid 3; widening is INVAL.
    nfsv4::Stateid after_dg;
    EXPECT_EQ(co_await mgr.open_downgrade(o4.stateid, a.clientid, state::kShareRead, 0,
                                          &after_dg), kOk);
    EXPECT_EQ(after_dg.seqid, 3u);
    EXPECT_EQ(co_await mgr.open_downgrade(after_dg, a.clientid, state::kShareBoth, 0,
                                          nullptr), st4(nfsv4::Status::kInval));
    // Now B may open for WRITE (A no longer denies).
    auto o6 = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(o6.status, kOk);
    EXPECT_TRUE(o6.merged);

    // CLOSE discipline: old -> OLD_STATEID, exact -> OK, again -> BAD; seqid 0 = current.
    nfsv4::Stateid out;
    EXPECT_EQ(co_await mgr.close_state(o4.stateid, a.clientid, &out),
              st4(nfsv4::Status::kOldStateid));
    EXPECT_EQ(co_await mgr.close_state(after_dg, a.clientid, &out), kOk);
    EXPECT_EQ(out.seqid, 4u);
    EXPECT_EQ(co_await mgr.close_state(after_dg, a.clientid, &out),
              st4(nfsv4::Status::kBadStateid));
    nfsv4::Stateid b_zero = o6.stateid;
    b_zero.seqid = 0;
    EXPECT_EQ(co_await mgr.close_state(b_zero, b.clientid, &out), kOk);
    auto s = mgr.stats();
    EXPECT_EQ(s.opens, 0u);
    EXPECT_EQ(s.files, 0u);
    EXPECT_EQ(s.share_denied, 2u);
    EXPECT_EQ(s.open_merges, 2u);
    // DESTROY_CLIENTID refuses while sessions exist, then succeeds.
    EXPECT_EQ(co_await mgr.destroy_session(a.sessionid, 1), kOk);
    EXPECT_EQ(co_await mgr.destroy_clientid(a.clientid), kOk);
  });
  runtime.stop_and_join();
}

TEST(StateMgr, CourtesyConflictAndTimeoutReclaim) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  // 1s lease, courtesy window = 1 × lease.
  state::StateMgr mgr({.boot_epoch = 5, .state_dir = dir.path, .lease_seconds = 1,
                       .courtesy_multiplier = 1});
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "client-a", 1);
    auto b = co_await connect(mgr, "client-b", 2);
    auto oa = co_await mgr.open(open_args(a.clientid, 1, "oa", state::kShareRead,
                                          state::kShareWrite), nullptr);
    EXPECT_EQ(oa.status, kOk);
    // Within the lease: B is denied.
    auto ob = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(ob.status, st4(nfsv4::Status::kShareDenied));

    // A goes silent; B keeps its lease alive.
    co_await rt::sleep_for(std::chrono::milliseconds(2100));
    auto seq = co_await mgr.sequence_begin(b.sessionid, 0, 1, 0, false, 1);
    EXPECT_EQ(seq.status, kOk);
    co_await mgr.sequence_complete(b.sessionid, 0, 1, false, {});
    co_await mgr.scan_leases();
    auto s1 = mgr.stats();
    EXPECT_EQ(s1.courtesy, 1u);
    EXPECT_EQ(s1.lease_expirations, 1u);
    EXPECT_EQ(s1.opens, 1u);  // courtesy keeps the state

    // Conflict path: B's WRITE open reclaims A and proceeds.
    auto ob2 = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareWrite, 0), nullptr);
    EXPECT_EQ(ob2.status, kOk);
    auto s2 = mgr.stats();
    EXPECT_EQ(s2.reclaim_conflict, 1u);
    EXPECT_EQ(s2.courtesy, 0u);
    EXPECT_EQ(s2.opens, 1u);
    EXPECT_EQ(s2.sessions, 1u);  // A's session is gone
    auto dead = co_await mgr.sequence_begin(a.sessionid, 0, 1, 0, false, 1);
    EXPECT_EQ(dead.status, st4(nfsv4::Status::kBadsession));
    auto stale = co_await mgr.check_io(oa.stateid, a.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(stale.status, st4(nfsv4::Status::kBadStateid));

    // Timeout path: B goes silent past lease + courtesy window.
    co_await rt::sleep_for(std::chrono::milliseconds(2100));
    co_await mgr.scan_leases();
    EXPECT_EQ(mgr.stats().courtesy, 1u);
    co_await rt::sleep_for(std::chrono::milliseconds(1100));
    co_await mgr.scan_leases();
    auto s3 = mgr.stats();
    EXPECT_EQ(s3.reclaim_timeout, 1u);
    EXPECT_EQ(s3.courtesy, 0u);
    EXPECT_EQ(s3.opens, 0u);
    EXPECT_EQ(s3.files, 0u);
    EXPECT_EQ(s3.sessions, 0u);
    EXPECT_EQ(s3.clients, 0u);
    EXPECT_FALSE(mgr.in_stable_list("client-b"));  // removed from the stable list too

    // Forced reclaim via the ctl path.
    auto c = co_await connect(mgr, "client-c", 3);
    auto oc = co_await mgr.open(open_args(c.clientid, 2, "oc", state::kShareBoth, 0), nullptr);
    EXPECT_EQ(oc.status, kOk);
    EXPECT_EQ(co_await mgr.expire_client(c.clientid), kOk);
    EXPECT_EQ(co_await mgr.expire_client(c.clientid), st4(nfsv4::Status::kStaleClientid));
    auto s4 = mgr.stats();
    EXPECT_EQ(s4.reclaim_forced, 1u);
    EXPECT_EQ(s4.opens, 0u);
    EXPECT_EQ(s4.clients, 0u);
    // A courtesy client that comes back in time revives (SEQUENCE renews).
    auto d = co_await connect(mgr, "client-d", 4);
    (void)co_await mgr.open(open_args(d.clientid, 3, "od", state::kShareRead, 0), nullptr);
    co_await rt::sleep_for(std::chrono::milliseconds(1100));
    co_await mgr.scan_leases();
    EXPECT_EQ(mgr.stats().courtesy, 1u);
    auto revive = co_await mgr.sequence_begin(d.sessionid, 0, 1, 0, false, 1);
    EXPECT_EQ(revive.status, kOk);
    co_await mgr.sequence_complete(d.sessionid, 0, 1, false, {});
    EXPECT_EQ(mgr.stats().courtesy, 0u);
    EXPECT_EQ(mgr.stats().opens, 1u);
    auto dump = co_await mgr.dump();
    EXPECT_TRUE(dump.find("client-d") == std::string::npos);  // owner shown as hex
    EXPECT_TRUE(dump.find("open ") != std::string::npos);
  });
  runtime.stop_and_join();
}

TEST(StateMgr, GraceReclaimGate) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  {
    state::StateMgr first({.boot_epoch = 1, .state_dir = dir.path});
    run_on(runtime, [&]() -> rt::Task<void> {
      auto a = co_await connect(first, "listed", 1);
      auto o = co_await first.open(open_args(a.clientid, 1, "oa", state::kShareBoth, 0), nullptr);
      EXPECT_EQ(o.status, kOk);
    });
  }
  state::StateMgr second({.boot_epoch = 2, .state_dir = dir.path});
  second.load_grace_list();
  EXPECT_TRUE(second.in_grace());
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(second, "listed", 1, false);  // reclaims first
    auto n = co_await connect(second, "newcomer", 2);
    // Pre-restart stateid: STALE_STATEID without a table walk.
    nfsv4::Stateid old{};
    uint32_t epoch = 1;
    std::memcpy(old.other.data(), &epoch, 4);
    old.other[5] = std::byte{1};
    old.seqid = 1;
    auto c = co_await second.check_io(old, a.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(c.status, st4(nfsv4::Status::kStaleStateid));
    // Non-reclaim OPEN during grace -> GRACE; anonymous WRITE -> GRACE; reads pass.
    auto plain = co_await second.open(open_args(n.clientid, 1, "on", state::kShareRead, 0), nullptr);
    EXPECT_EQ(plain.status, st4(nfsv4::Status::kGrace));
    nfsv4::Stateid anon{};
    auto w = co_await second.check_io(anon, n.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(w.status, st4(nfsv4::Status::kGrace));
    auto r = co_await second.check_io(anon, n.clientid, 1, oid_of(1), state::kShareRead);
    EXPECT_EQ(r.status, kOk);
    // Unlisted client reclaiming -> RECLAIM_BAD; listed -> OK.
    auto bad = open_args(n.clientid, 1, "on", state::kShareRead, 0);
    bad.reclaim = true;
    EXPECT_EQ((co_await second.open(bad, nullptr)).status, st4(nfsv4::Status::kReclaimBad));
    auto good = open_args(a.clientid, 1, "oa", state::kShareBoth, 0);
    good.reclaim = true;
    auto reclaimed = co_await second.open(good, nullptr);
    EXPECT_EQ(reclaimed.status, kOk);
    EXPECT_TRUE(second.in_grace());
    // RECLAIM_COMPLETE from every listed client ends grace early; reclaim afterwards is
    // NO_GRACE, plain OPENs flow.
    EXPECT_EQ(co_await second.reclaim_complete(a.clientid), kOk);
    EXPECT_FALSE(second.in_grace());
    EXPECT_EQ((co_await second.open(good, nullptr)).status, st4(nfsv4::Status::kNoGrace));
    EXPECT_EQ((co_await second.open(open_args(n.clientid, 1, "on", state::kShareRead, 0), nullptr)).status, kOk);
  });
  runtime.stop_and_join();
}

TEST(StateMgr, ByteRangeLocksLifecycle) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 5, .state_dir = dir.path, .lease_seconds = 1,
                       .courtesy_multiplier = 1});
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "client-a", 1);
    auto b = co_await connect(mgr, "client-b", 2);
    auto oa = co_await mgr.open(open_args(a.clientid, 1, "oa", state::kShareBoth, 0), nullptr);
    auto ob = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareRead, 0), nullptr);
    EXPECT_EQ(oa.status, kOk);
    EXPECT_EQ(ob.status, kOk);

    state::StateMgr::LockArgs la;
    la.clientid = a.clientid;
    la.fsid = 1;
    la.oid = oid_of(1);
    la.exclusive = true;
    la.offset = 0;
    la.length = 100;
    la.new_owner = true;
    la.open_stateid = oa.stateid;
    la.owner = "proc-a";
    auto l1 = co_await mgr.lock(la);
    EXPECT_EQ(l1.status, kOk);
    EXPECT_EQ(l1.stateid.seqid, 1u);
    EXPECT_EQ(l1.stateid.other[4], std::byte{2});  // type byte = kLock
    // b (read-only open) asking for a write lock -> OPENMODE; a read lock over a's
    // exclusive range -> DENIED naming a's owner; outside the range -> OK.
    state::StateMgr::LockArgs lb = la;
    lb.clientid = b.clientid;
    lb.open_stateid = ob.stateid;
    lb.owner = "proc-b";
    EXPECT_EQ((co_await mgr.lock(lb)).status, st4(nfsv4::Status::kOpenmode));
    lb.exclusive = false;
    auto denied = co_await mgr.lock(lb);
    EXPECT_EQ(denied.status, st4(nfsv4::Status::kDenied));
    EXPECT_EQ(denied.denied.clientid, a.clientid);
    EXPECT_STREQ(denied.denied.owner, "proc-a");
    EXPECT_EQ(denied.denied.length, 100u);
    EXPECT_TRUE(denied.denied.exclusive);
    EXPECT_EQ(mgr.stats().lock_states, 1u);  // no stateid minted for a denied new owner
    lb.offset = 100;
    auto l2 = co_await mgr.lock(lb);
    EXPECT_EQ(l2.status, kOk);
    // LOCKT from b over a's range -> DENIED; from a itself -> OK (own locks ignored).
    auto t1 = co_await mgr.lockt(b.clientid, 1, oid_of(1), "proc-b", false, 0, 10);
    EXPECT_EQ(t1.status, st4(nfsv4::Status::kDenied));
    auto t2 = co_await mgr.lockt(a.clientid, 1, oid_of(1), "proc-a", true, 0, 10);
    EXPECT_EQ(t2.status, kOk);
    // Existing lock stateid path bumps seqid; stale seqid is OLD_STATEID.
    la.new_owner = false;
    la.lock_stateid = l1.stateid;
    la.offset = 200;
    auto l3 = co_await mgr.lock(la);
    EXPECT_EQ(l3.status, kOk);
    EXPECT_TRUE(l3.stateid.other == l1.stateid.other);
    EXPECT_EQ(l3.stateid.seqid, 2u);
    EXPECT_EQ((co_await mgr.lock(la)).status, st4(nfsv4::Status::kOldStateid));
    // IO through a lock stateid carries the open's mode.
    auto io = co_await mgr.check_io(l3.stateid, a.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(io.status, kOk);
    auto io2 = co_await mgr.check_io(l2.stateid, b.clientid, 1, oid_of(1), state::kShareWrite);
    EXPECT_EQ(io2.status, st4(nfsv4::Status::kOpenmode));
    // FREE_STATEID: LOCKS_HELD while ranges remain; LOCKU everything, then frees.
    EXPECT_EQ(co_await mgr.free_stateid(l3.stateid), st4(nfsv4::Status::kLocksHeld));
    nfsv4::Stateid after;
    EXPECT_EQ(co_await mgr.locku(l3.stateid, a.clientid, 0, UINT64_MAX, &after), kOk);
    EXPECT_EQ(after.seqid, 3u);
    EXPECT_EQ(co_await mgr.locku(l3.stateid, b.clientid, 0, 1, nullptr),
              st4(nfsv4::Status::kBadStateid));
    EXPECT_EQ(co_await mgr.free_stateid(after), kOk);
    EXPECT_EQ(mgr.stats().lock_states, 1u);
    // CLOSE releases b's lock state and its ranges.
    nfsv4::Stateid closed;
    EXPECT_EQ(co_await mgr.close_state(ob.stateid, b.clientid, &closed), kOk);
    auto s = mgr.stats();
    EXPECT_EQ(s.lock_states, 0u);
    EXPECT_EQ(s.lock_segments, 0u);
    EXPECT_EQ(s.files, 1u);
    EXPECT_EQ(s.lock_denied, 1u);

    // Courtesy conflict through a lock: a takes a lock, goes silent, b's lock reclaims.
    la.new_owner = true;
    la.open_stateid = oa.stateid;
    la.offset = 0;
    la.length = 10;
    EXPECT_EQ((co_await mgr.lock(la)).status, kOk);
    auto ob2 = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareBoth, 0), nullptr);
    EXPECT_EQ(ob2.status, kOk);
    lb.open_stateid = ob2.stateid;
    lb.new_owner = true;
    lb.offset = 0;
    lb.length = 10;
    lb.exclusive = true;
    EXPECT_EQ((co_await mgr.lock(lb)).status, st4(nfsv4::Status::kDenied));
    co_await rt::sleep_for(std::chrono::milliseconds(2100));
    auto seq = co_await mgr.sequence_begin(b.sessionid, 0, 1, 0, false, 1);
    EXPECT_EQ(seq.status, kOk);
    co_await mgr.sequence_complete(b.sessionid, 0, 1, false, {});
    co_await mgr.scan_leases();
    EXPECT_EQ(mgr.stats().courtesy, 1u);
    EXPECT_EQ((co_await mgr.lock(lb)).status, kOk);
    auto s2 = mgr.stats();
    EXPECT_EQ(s2.reclaim_conflict, 1u);
    EXPECT_EQ(s2.opens, 1u);
    EXPECT_EQ(s2.lock_states, 1u);
    auto dump = co_await mgr.dump();
    EXPECT_TRUE(dump.find("lock ") != std::string::npos);
    EXPECT_TRUE(dump.find("[0,10)W") != std::string::npos);
  });
  runtime.stop_and_join();
}

// Targeted reproducer for plan doc 10 §1.2: IO through a lock stateid copies the parent
// open's backend handle, while a concurrent CLOSE of that open moves the handle out
// under the parent's shard lock — the copy must hold that same lock. The two sides run
// on different reactors; TSAN builds flag the unlocked copy, and the loop shape keeps
// each round's copy/move pair genuinely unordered.
TEST(StateMgr, LockStateidIoRacesParentClose) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 2, .offload_threads = 2});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 5, .state_dir = dir.path});
  // Hang guard: mini_test has no per-test timeout, and this test's first find was a
  // reactor stall (stale block timeout vs a timer armed mid-pump), which froze the
  // whole suite rather than failing.
  std::atomic<bool> test_done{false};
  std::thread watchdog([&] {
    for (int i = 0; i < 600 && !test_done.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!test_done.load()) {
      fprintf(stderr, "LockStateidIoRacesParentClose: no progress in 60s; aborting\n");
      std::abort();
    }
  });
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "client-a", 1);
    for (int round = 0; round < 100; ++round) {
      auto owner = "own-" + std::to_string(round);
      auto args = open_args(a.clientid, 1, owner.c_str(), state::kShareBoth, 0);
      auto o = co_await mgr.open(std::move(args), std::make_shared<backend::OpenState>());
      EXPECT_EQ(o.status, kOk);
      state::StateMgr::LockArgs la;
      la.clientid = a.clientid;
      la.fsid = 1;
      la.oid = oid_of(1);
      la.exclusive = true;
      la.offset = 0;
      la.length = 10;
      la.new_owner = true;
      la.open_stateid = o.stateid;
      la.owner = "proc-" + std::to_string(round);
      auto l = co_await mgr.lock(la);
      EXPECT_EQ(l.status, kOk);

      std::atomic<bool> closed{false};
      rt::spawn(
          [](state::StateMgr* mgr, nfsv4::Stateid sid, uint64_t clientid,
             std::atomic<bool>* closed) -> rt::Task<void> {
            nfsv4::Stateid out;
            co_await mgr->close_state(sid, clientid, &out);
            closed->store(true, std::memory_order_release);
          }(&mgr, o.stateid, a.clientid, &closed),
          runtime.reactor(1));
      // Hammer the lock stateid until the close cascade unlinks it or the spin budget
      // runs out; each successful check copies the parent handle. The final copy of a
      // round is never ordered against the close's move-out, so even when the tight
      // loop starves the closer until the budget is spent, every round still puts one
      // unsynchronized copy/move pair in front of TSAN.
      for (int spin = 0; spin < 200; ++spin) {
        auto io = co_await mgr.check_io(l.stateid, a.clientid, 1, oid_of(1),
                                        state::kShareWrite);
        if (io.status != kOk) break;
      }
      while (!closed.load(std::memory_order_acquire))
        co_await rt::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(mgr.stats().opens, 0u);
    EXPECT_EQ(mgr.stats().lock_states, 0u);
  });
  test_done.store(true);
  watchdog.join();
  runtime.stop_and_join();
}

// Plan doc 10 §1.5: resource caps answer NFS4ERR_RESOURCE instead of letting a runaway
// client grow the tables without bound, and the lock-owner resolution table shrinks
// again when its client goes away.
TEST(StateMgr, ResourceCapsReturnResource) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 9,
                       .state_dir = dir.path,
                       .max_clients = 2,
                       .max_states_per_client = 2,
                       .max_lock_segments_per_owner = 2});
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "cap-a", 1);
    auto b = co_await connect(mgr, "cap-b", 2);
    EXPECT_TRUE(a.clientid != 0 && b.clientid != 0);

    // Third distinct client: over max_clients.
    nfsv4::Verifier verf{};
    verf[0] = std::byte{3};
    auto over = co_await mgr.exchange_id("cap-c", verf, "sys/t/0", false);
    EXPECT_EQ(over.status, st4(nfsv4::Status::kResource));
    // A re-registration of an existing owner is not a new client and still works.
    verf[0] = std::byte{1};
    auto again = co_await mgr.exchange_id("cap-a", verf, "sys/t/0", false);
    EXPECT_EQ(again.status, kOk);

    // Two opens fill the per-client state cap; the third answers RESOURCE.
    auto o1 = co_await mgr.open(open_args(a.clientid, 1, "ow", state::kShareRead, 0), nullptr);
    auto o2 = co_await mgr.open(open_args(a.clientid, 2, "ow", state::kShareRead, 0), nullptr);
    EXPECT_EQ(o1.status, kOk);
    EXPECT_EQ(o2.status, kOk);
    auto o3 = co_await mgr.open(open_args(a.clientid, 3, "ow", state::kShareRead, 0), nullptr);
    EXPECT_EQ(o3.status, st4(nfsv4::Status::kResource));
    // The other client is unaffected by a's cap.
    auto ob = co_await mgr.open(open_args(b.clientid, 3, "ow", state::kShareBoth, 0), nullptr);
    EXPECT_EQ(ob.status, kOk);

    // Lock segment fragmentation: two disjoint exclusive ranges fill the per-owner
    // cap on the file; the third answers RESOURCE.
    state::StateMgr::LockArgs la;
    la.clientid = b.clientid;
    la.fsid = 1;
    la.oid = oid_of(3);
    la.exclusive = true;
    la.new_owner = true;
    la.open_stateid = ob.stateid;
    la.owner = "lo";
    la.offset = 0;
    la.length = 10;
    auto l1 = co_await mgr.lock(la);
    EXPECT_EQ(l1.status, kOk);
    state::StateMgr::LockArgs la2 = la;
    la2.new_owner = false;
    la2.lock_stateid = l1.stateid;
    la2.offset = 20;
    auto l2 = co_await mgr.lock(la2);
    EXPECT_EQ(l2.status, kOk);
    state::StateMgr::LockArgs la3 = la2;
    la3.lock_stateid = l2.stateid;
    la3.offset = 40;
    auto l3 = co_await mgr.lock(la3);
    EXPECT_EQ(l3.status, st4(nfsv4::Status::kResource));

    // The lock-owner table shrinks with its client (was insert-only).
    EXPECT_TRUE(mgr.stats().lock_owners >= 1);
    EXPECT_EQ(co_await mgr.expire_client(b.clientid), kOk);
    EXPECT_EQ(mgr.stats().lock_owners, 0u);
  });
  runtime.stop_and_join();
}

// Plan doc 10 §2.6: shard count is configurable; a single-shard table must serve the
// full client/session lifecycle (also exercises the clientid index + per-slot waits
// with everything hashed into one shard).
TEST(StateMgr, SingleShardConfigLifecycle) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  state::StateMgr mgr({.boot_epoch = 7, .state_dir = dir.path, .shards = 1});

  std::atomic<bool> done{false};
  std::atomic<int> errors{0};
  rt::spawn(
      [](state::StateMgr* mgr, std::atomic<bool>* done,
         std::atomic<int>* errors) -> rt::Task<void> {
        nfsv4::Verifier verf{};
        verf[0] = std::byte{0x11};
        auto ex = co_await mgr->exchange_id("single-shard", verf, "sys/test/0", false);
        if (ex.status != 0) ++*errors;
        nfsv4::ChannelAttrs attrs;
        attrs.max_requests = 4;
        auto cs = co_await mgr->create_session(ex.clientid, ex.sequenceid, "sys/test/0",
                                               attrs, attrs, 1);
        if (cs.status != 0) ++*errors;
        auto seq = co_await mgr->sequence_begin(cs.sessionid, 0, 1, 0, false, 1);
        if (seq.status != 0 || seq.replay) ++*errors;
        co_await mgr->sequence_complete(cs.sessionid, 0, 1, false, {});
        // Replay of the completed slot classifies as replay, not misordered.
        auto replay = co_await mgr->sequence_begin(cs.sessionid, 0, 1, 0, false, 1);
        if (!replay.replay) ++*errors;
        if ((co_await mgr->destroy_session(cs.sessionid, 1)) != 0) ++*errors;
        if ((co_await mgr->destroy_clientid(ex.clientid)) != 0) ++*errors;
        // The index must agree: the clientid is gone.
        if ((co_await mgr->destroy_clientid(ex.clientid)) !=
            static_cast<uint32_t>(nfsv4::Status::kStaleClientid))
          ++*errors;
        done->store(true);
      }(&mgr, &done, &errors),
      runtime.reactor(0));
  while (!done.load()) std::this_thread::yield();
  runtime.stop_and_join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(mgr.stats().clients, 0u);
  EXPECT_EQ(mgr.stats().sessions, 0u);
}

// Decoupled grace window ([protocol] grace, plan doc 10 §4.4) and the operator's
// `grace-end` override (plan doc 10 §4.2).
TEST(StateMgr, GraceDecoupledFromLeaseAndOperatorEnd) {
  TmpDir dir;
  std::filesystem::create_directories(dir.path + "/clients");
  if (FILE* f = fopen((dir.path + "/clients/c1").c_str(), "wb")) {
    fputs("ownerX", f);
    fclose(f);
  }
  state::StateMgr mgr({.boot_epoch = 2,
                       .state_dir = dir.path,
                       .lease_seconds = 90,
                       .grace_seconds = 5});
  mgr.load_grace_list();
  EXPECT_TRUE(mgr.in_grace());
  // The window follows grace_seconds (5), not the 90s lease.
  EXPECT_TRUE(mgr.grace_remaining_seconds() <= 5);
  EXPECT_TRUE(mgr.end_grace());
  EXPECT_FALSE(mgr.in_grace());
  EXPECT_FALSE(mgr.end_grace());  // second call: nothing left to end
}

namespace {

// A backend lock manager for the native push path (design 05 §5.8, plan doc 10
// §5.3): records every call, refuses lock() when told to and answers test() with a
// configurable conflict — what a gateway sees when another gateway holds the range.
struct FakeNativeLocks final : backend::LockMgr {
  std::vector<std::string> calls;
  Errno fail = Errno::kOk;                          // lock() outcome when != kOk
  std::optional<backend::LockConflict> conflict{};  // test() answer

  rt::Task<Result<void>> lock(backend::Object&, const backend::LockOwnerId&,
                              backend::LockRange r, bool exclusive, bool) override {
    calls.push_back(std::format("lock {} {} {}", r.offset, r.length, exclusive ? 1 : 0));
    if (fail != Errno::kOk) co_return Err(fail);
    co_return Result<void>{};
  }
  rt::Task<Result<void>> unlock(backend::Object&, const backend::LockOwnerId&,
                                backend::LockRange r) override {
    calls.push_back(std::format("unlock {} {}", r.offset, r.length));
    co_return Result<void>{};
  }
  rt::Task<Result<std::optional<backend::LockConflict>>> test(backend::Object&,
                                                              backend::LockRange,
                                                              bool) override {
    calls.push_back("test");
    co_return conflict;
  }
  rt::Task<Result<void>> release(backend::Object&, const backend::LockOwnerId&) override {
    calls.push_back("release");
    co_return Result<void>{};
  }
};

}  // namespace

TEST(StateMgr, NativeLockPushRollbackAndRelease) {
  TmpDir dir;
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  FakeNativeLocks native;
  backend::MemoryBackend memory(1);
  state::StateMgr::Config cfg{.boot_epoch = 5, .state_dir = dir.path, .lease_seconds = 1,
                              .courtesy_multiplier = 1};
  cfg.native_locks.manager = [&](uint32_t fsid) -> backend::LockMgr* {
    return fsid == 1 ? &native : nullptr;
  };
  cfg.native_locks.resolve = [&](uint32_t, const backend::ObjId&)
      -> rt::Task<Result<backend::ObjPtr>> { co_return co_await memory.root(); };
  state::StateMgr mgr(cfg);
  run_on(runtime, [&]() -> rt::Task<void> {
    auto a = co_await connect(mgr, "client-a", 1);
    auto b = co_await connect(mgr, "client-b", 2);
    auto oa = co_await mgr.open(open_args(a.clientid, 1, "oa", state::kShareBoth, 0), nullptr);
    auto ob = co_await mgr.open(open_args(b.clientid, 1, "ob", state::kShareBoth, 0), nullptr);
    EXPECT_EQ(oa.status, kOk);
    EXPECT_EQ(ob.status, kOk);
    state::FileKey key{1, oid_of(1)};

    // Granted locally and pushed to the storage with the same range/type.
    state::StateMgr::LockArgs la;
    la.clientid = a.clientid;
    la.fsid = 1;
    la.oid = oid_of(1);
    la.exclusive = true;
    la.offset = 0;
    la.length = 100;
    la.new_owner = true;
    la.open_stateid = oa.stateid;
    la.owner = "proc-a";
    auto l1 = co_await mgr.lock(la);
    EXPECT_EQ(l1.status, kOk);
    EXPECT_EQ(native.calls.size(), 1u);
    EXPECT_STREQ(native.calls.back(), "lock 0 100 1");

    // The storage refuses an extension (another gateway holds [50,75)): DENIED with
    // the storage's conflict (holder unknown → clientid 0), and the gateway grant is
    // rolled back to the previous coverage, [0,100) exactly.
    native.fail = errno_from(EAGAIN);
    native.conflict = backend::LockConflict{{}, {50, 25}, true};
    la.new_owner = false;
    la.lock_stateid = l1.stateid;
    la.offset = 50;
    la.length = 200;
    auto denied = co_await mgr.lock(la);
    EXPECT_EQ(denied.status, st4(nfsv4::Status::kDenied));
    EXPECT_EQ(denied.denied.offset, 50u);
    EXPECT_EQ(denied.denied.length, 25u);
    EXPECT_TRUE(denied.denied.exclusive);
    EXPECT_EQ(denied.denied.clientid, 0u);
    EXPECT_TRUE(denied.denied.owner.empty());
    auto segs = mgr.lock_table().segments(key);
    EXPECT_EQ(segs.size(), 1u);
    if (!segs.empty()) {
      EXPECT_EQ(segs[0].start, 0u);
      EXPECT_EQ(segs[0].end, 100u);
    }
    EXPECT_EQ(mgr.stats().native_lock_denied, 1u);
    EXPECT_EQ(mgr.stats().lock_states, 1u);
    // A new owner refused by the storage mints no stateid.
    state::StateMgr::LockArgs lb = la;
    lb.clientid = b.clientid;
    lb.new_owner = true;
    lb.open_stateid = ob.stateid;
    lb.owner = "proc-b";
    lb.offset = 500;
    lb.length = 10;
    EXPECT_EQ((co_await mgr.lock(lb)).status, st4(nfsv4::Status::kDenied));
    EXPECT_EQ(mgr.stats().lock_states, 1u);
    EXPECT_EQ(mgr.lock_table().segments(key).size(), 1u);
    // Storage trouble answers DELAY and grants nothing.
    native.fail = Errno::kJukebox;
    EXPECT_EQ((co_await mgr.lock(lb)).status, st4(nfsv4::Status::kDelay));
    EXPECT_EQ(mgr.stats().native_lock_errors, 1u);
    EXPECT_EQ(mgr.lock_table().segments(key).size(), 1u);
    native.fail = Errno::kOk;

    // LOCKT: no local conflict → the storage is probed; a probe that reports a
    // holder is DENIED with that range.  An owner already covering part of the range
    // is answered by the gateway table alone (the probe cannot tell own locks apart).
    native.conflict = backend::LockConflict{{}, {700, 5}, false};
    auto t1 = co_await mgr.lockt(b.clientid, 1, oid_of(1), "proc-b", true, 700, 10);
    EXPECT_EQ(t1.status, st4(nfsv4::Status::kDenied));
    EXPECT_EQ(t1.denied.offset, 700u);
    EXPECT_EQ(t1.denied.length, 5u);
    EXPECT_STREQ(native.calls.back(), "test");
    size_t before = native.calls.size();
    auto t2 = co_await mgr.lockt(a.clientid, 1, oid_of(1), "proc-a", true, 10, 10);
    EXPECT_EQ(t2.status, kOk);
    EXPECT_EQ(native.calls.size(), before);
    native.conflict.reset();
    auto t3 = co_await mgr.lockt(b.clientid, 1, oid_of(1), "proc-b", false, 900, 10);
    EXPECT_EQ(t3.status, kOk);
    EXPECT_STREQ(native.calls.back(), "test");

    // LOCKU mirrors the release; CLOSE drops the owner's locks in the storage too.
    nfsv4::Stateid out{};
    EXPECT_EQ(co_await mgr.locku(l1.stateid, a.clientid, 0, 50, &out), kOk);
    EXPECT_STREQ(native.calls.back(), "unlock 0 50");
    EXPECT_EQ(co_await mgr.close_state(oa.stateid, a.clientid, &out), kOk);
    EXPECT_STREQ(native.calls.back(), "release");
    EXPECT_EQ(mgr.lock_table().segments(key).size(), 0u);
  });
  runtime.stop_and_join();
}
