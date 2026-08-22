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
#include <mutex>

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
