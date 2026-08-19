// Sync primitive semantics on a single FakeRing reactor: FIFO fairness, lock-across-await,
// shared-lock batching, condvar wakeup ordering, semaphore permit transfer.

#include <string>
#include <vector>

#include "mini_test.hpp"
#include "runtime/reactor.hpp"
#include "runtime/sync.hpp"
#include "runtime/task.hpp"
#include "runtime/testing/fake_ring.hpp"

using namespace lnfs::rt;
using testing::FakeRing;

namespace {
struct Ctx {
  AsyncMutex mu;
  AsyncSharedMutex smu;
  AsyncCondVar cv;
  Semaphore sem{2};
  Event ev;
  std::vector<std::string> trace;
  int shared_counter = 0;
  bool ready = false;
};

Task<void> locker(Ctx* c, std::string name) {
  auto lk = co_await c->mu.lock();
  c->trace.push_back(name + ":enter");
  // hold across a real suspension point: everyone else must queue
  co_await c->ev.wait();
  c->trace.push_back(name + ":exit");
}
}  // namespace

TEST(AsyncMutex, FifoFairness) {
  FakeRing ring;
  Reactor r(ring);
  Ctx c;
  spawn(locker(&c, "a"), r);
  spawn(locker(&c, "b"), r);
  spawn(locker(&c, "c"), r);
  while (r.poll_once()) {
  }
  // a holds the lock and waits on the event; b and c are queued on the mutex.
  ASSERT_TRUE(c.trace.size() == 1);
  c.ev.set();
  while (r.poll_once()) {
  }
  ASSERT_TRUE(c.trace.size() == 6);
  EXPECT_STREQ(c.trace[0], "a:enter");
  EXPECT_STREQ(c.trace[1], "a:exit");
  EXPECT_STREQ(c.trace[2], "b:enter");
  EXPECT_STREQ(c.trace[3], "b:exit");
  EXPECT_STREQ(c.trace[4], "c:enter");
  EXPECT_STREQ(c.trace[5], "c:exit");
}

TEST(AsyncSharedMutex, ReadersSharedWriterExclusive) {
  FakeRing ring;
  Reactor r(ring);
  Ctx c;
  // reader1, reader2 grab shared; writer queues; reader3 arrives after writer and must
  // queue behind it (FIFO — no writer starvation).
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->smu.lock_shared();
        cc->trace.push_back("r1");
        co_await cc->ev.wait();  // hold until released
      }(&c),
      r);
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->smu.lock_shared();
        cc->trace.push_back("r2");
        co_await cc->ev.wait();
      }(&c),
      r);
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->smu.lock();
        cc->trace.push_back("w");
      }(&c),
      r);
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->smu.lock_shared();
        cc->trace.push_back("r3");
      }(&c),
      r);
  while (r.poll_once()) {
  }
  ASSERT_TRUE(c.trace.size() == 2);  // both readers in; writer + r3 queued
  c.ev.set();                        // release both readers
  while (r.poll_once()) {
  }
  ASSERT_TRUE(c.trace.size() == 4);
  EXPECT_STREQ(c.trace[2], "w");
  EXPECT_STREQ(c.trace[3], "r3");
}

TEST(AsyncCondVar, WaitNotify) {
  FakeRing ring;
  Reactor r(ring);
  Ctx c;
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->mu.lock();
        while (!cc->ready) co_await cc->cv.wait(cc->mu, lk);
        cc->trace.push_back("consumer");
      }(&c),
      r);
  while (r.poll_once()) {
  }
  EXPECT_TRUE(c.trace.empty());
  spawn(
      [](Ctx* cc) -> Task<void> {
        auto lk = co_await cc->mu.lock();
        cc->ready = true;
        cc->cv.notify_one();
        cc->trace.push_back("producer");
      }(&c),
      r);
  while (r.poll_once()) {
  }
  ASSERT_TRUE(c.trace.size() == 2);
  EXPECT_STREQ(c.trace[0], "producer");
  EXPECT_STREQ(c.trace[1], "consumer");
}

TEST(Semaphore, PermitTransfer) {
  FakeRing ring;
  Reactor r(ring);
  Ctx c;  // sem = 2
  for (int i = 0; i < 4; ++i) {
    spawn(
        [](Ctx* cc, int idx) -> Task<void> {
          co_await cc->sem.acquire();
          cc->trace.push_back("go" + std::to_string(idx));
          co_await cc->ev.wait();
          cc->sem.release();
        }(&c, i),
        r);
  }
  while (r.poll_once()) {
  }
  EXPECT_EQ(c.trace.size(), 2u);  // only 2 permits
  c.ev.set();
  while (r.poll_once()) {
  }
  EXPECT_EQ(c.trace.size(), 4u);
  EXPECT_EQ(c.sem.available(), 2);
}

TEST(Sharded, IndependentShards) {
  FakeRing ring;
  Reactor r(ring);
  Sharded<int, 4> tab;
  bool done = false;
  spawn(
      [](Sharded<int, 4>* t, bool* d) -> Task<void> {
        // Locks on different shards don't contend.
        auto& s1 = t->shard(1);
        auto& s2 = t->shard(2);
        auto l1 = co_await s1.mu.lock();
        auto l2 = co_await s2.mu.lock();
        s1.v = 10;
        s2.v = 20;
        *d = true;
      }(&tab, &done),
      r);
  while (r.poll_once()) {
  }
  EXPECT_TRUE(done);
  EXPECT_EQ(tab.shard(1).v, 10);
  EXPECT_EQ(tab.shard(2).v, 20);
  EXPECT_EQ(tab.shard(5).v, 10);  // same shard as 1 (mod 4)
}
