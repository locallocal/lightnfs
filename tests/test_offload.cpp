// offload(): runs on the pool thread, resumes on the originating reactor (the runtime's only
// cross-thread point, design 02 §2.2). Uses a real UringRing/EpollRing because FakeRing has
// no cross-thread wake; picks whichever ring is available.

#include <thread>

#include "mini_test.hpp"
#include "runtime/offload_pool.hpp"
#include "runtime/reactor.hpp"
#include "runtime/runtime.hpp"

using namespace lnfs::rt;

TEST(Offload, RoundTripAffinity) {
  Runtime rt(Runtime::Config{.reactors = 2, .offload_threads = 2});
  std::thread::id reactor_tid{};
  std::thread::id offload_tid{};
  std::thread::id resume_tid{};

  spawn(
      [](std::thread::id* rt_id, std::thread::id* off_id, std::thread::id* res_id,
         Runtime* r) -> Task<void> {
        *rt_id = std::this_thread::get_id();
        int v = co_await offload([off_id] {
          *off_id = std::this_thread::get_id();
          return 41;
        });
        *res_id = std::this_thread::get_id();
        EXPECT_EQ(v, 41);
        r->reactor(0).stop();
        r->reactor(1).stop();
      }(&reactor_tid, &offload_tid, &resume_tid, &rt),
      rt.reactor(0));

  rt.start();
  rt.stop_and_join();  // reactors stop once tasks drain (stop() already requested inside)

  EXPECT_TRUE(reactor_tid == resume_tid);   // resumed back on the originating reactor
  EXPECT_TRUE(reactor_tid != offload_tid);  // work ran elsewhere
}

TEST(Offload, ExceptionPropagates) {
  Runtime rt(Runtime::Config{.reactors = 1, .offload_threads = 1});
  bool caught = false;
  spawn(
      [](bool* c, Runtime* r) -> Task<void> {
        try {
          co_await offload([]() -> int { throw std::runtime_error("boom"); });
        } catch (const std::runtime_error&) {
          *c = true;
        }
        r->reactor(0).stop();
      }(&caught, &rt),
      rt.reactor(0));
  rt.start();
  rt.stop_and_join();
  EXPECT_TRUE(caught);
}

TEST(Offload, ManyConcurrent) {
  Runtime rt(Runtime::Config{.reactors = 2, .offload_threads = 4});
  std::atomic<int> sum{0};
  for (size_t i = 0; i < 100; ++i) {
    spawn(
        [](std::atomic<int>* s) -> Task<void> {
          int v = co_await offload([] { return 1; });
          s->fetch_add(v);
        }(&sum),
        rt.next());
  }
  rt.start();
  // busy-wait for completion then stop
  while (sum.load() < 100) std::this_thread::yield();
  rt.stop_and_join();
  EXPECT_EQ(sum.load(), 100);
}

// ---- plan doc 10 §2.5: classes, admission cap, stats -------------------------

TEST(Offload, HeavyClassDoesNotBlockLight) {
  // 4 threads -> 3 light + 1 heavy. Park the lone heavy worker on a slow job, then
  // prove light jobs still run to completion while it is stuck.
  OffloadPool pool(OffloadPool::Config{.threads = 4});
  std::mutex mu;
  std::condition_variable cv;
  bool release_heavy = false;
  std::atomic<int> light_done{0};

  pool.submit(
      [&] {
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return release_heavy; });
      },
      OffloadClass::kHeavy);
  for (int i = 0; i < 8; ++i)
    pool.submit([&] { light_done.fetch_add(1); }, OffloadClass::kLight);

  for (int spin = 0; light_done.load() < 8 && spin < 2000; ++spin)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(light_done.load(), 8);

  {
    std::lock_guard lk(mu);
    release_heavy = true;
  }
  cv.notify_all();
  pool.stop_and_join();
  auto s = pool.stats();
  EXPECT_EQ(s.completed[0], 8u);
  EXPECT_EQ(s.completed[1], 1u);
}

TEST(Offload, AdmissionCapDefersButCompletesAll) {
  OffloadPool pool(OffloadPool::Config{.threads = 2, .heavy_threads = 1, .queue_cap = 2});
  std::atomic<int> done{0};
  std::mutex mu;
  std::condition_variable cv;
  bool release = false;
  // Park the single light worker so submissions pile up past the cap.
  pool.submit([&] {
    std::unique_lock lk(mu);
    cv.wait(lk, [&] { return release; });
  });
  for (int i = 0; i < 32; ++i) pool.submit([&] { done.fetch_add(1); });
  auto mid = pool.stats();
  EXPECT_TRUE(mid.deferred[0] > 0);        // cap engaged
  EXPECT_TRUE(pool.queue_depth() >= 32u);  // queued + overflow all accounted
  {
    std::lock_guard lk(mu);
    release = true;
  }
  cv.notify_all();
  pool.stop_and_join();  // drain: every deferred job still runs
  EXPECT_EQ(done.load(), 32);
  auto s = pool.stats();
  EXPECT_EQ(s.completed[0], 33u);
  EXPECT_EQ(s.depth[0], 0u);
}

TEST(Offload, SingleThreadPoolServesHeavy) {
  // threads=1: no separate heavy group; heavy jobs fall back to the light workers
  // instead of deadlocking.
  OffloadPool pool(OffloadPool::Config{.threads = 1});
  std::atomic<int> done{0};
  pool.submit([&] { done.fetch_add(1); }, OffloadClass::kHeavy);
  pool.submit([&] { done.fetch_add(1); }, OffloadClass::kLight);
  pool.stop_and_join();
  EXPECT_EQ(done.load(), 2);
}

TEST(Offload, HeavyClassAwaitRoundTrip) {
  Runtime rt(Runtime::Config{.reactors = 1, .offload_threads = 4});
  int got = 0;
  spawn(
      [](int* out, Runtime* r) -> Task<void> {
        *out = co_await offload([] { return 17; }, OffloadClass::kHeavy);
        r->reactor(0).stop();
      }(&got, &rt),
      rt.reactor(0));
  rt.start();
  rt.stop_and_join();
  EXPECT_EQ(got, 17);
}
