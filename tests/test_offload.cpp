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
