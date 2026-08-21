// StateMgr concurrency matrix (development plan §5.3, design 07 §7.2): storms of
// EXCHANGE_ID / CREATE_SESSION / SEQUENCE begin+complete / open+close / DESTROY_* from
// multiple reactors concurrently. Completion of this test is the deadlock-freedom
// assertion (single-shard-at-a-time locking); invariants check table consistency.

#include "mini_test.hpp"

#include <stdlib.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>

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
