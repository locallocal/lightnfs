// Timers and with_timeout on a deterministic fake clock: FakeRing::on_wait advances the
// injected clock by the reactor's requested block duration.

#include "mini_test.hpp"
#include "runtime/io.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "runtime/token_bucket.hpp"

using namespace lnfs::rt;
using namespace std::chrono_literals;
using testing::FakeRing;

namespace {
struct FakeClock {
  TimePoint now = TimePoint(std::chrono::seconds(1000));
};
}  // namespace

TEST(Timer, SleepOrdering) {
  FakeRing ring;
  FakeClock clk;
  Reactor::Options opts;
  opts.clock = [&clk] { return clk.now; };
  Reactor r(ring, opts);
  ring.on_wait = [&clk](std::optional<std::chrono::nanoseconds> t) {
    if (t && t->count() > 0) clk.now += *t;  // reactor blocks -> time passes
  };

  std::vector<int> order;
  spawn(
      [](std::vector<int>* o) -> Task<void> {
        co_await sleep_for(300ms);
        o->push_back(300);
      }(&order),
      r);
  spawn(
      [](std::vector<int>* o) -> Task<void> {
        co_await sleep_for(100ms);
        o->push_back(100);
      }(&order),
      r);
  spawn(
      [](std::vector<int>* o) -> Task<void> {
        co_await sleep_for(200ms);
        o->push_back(200);
      }(&order),
      r);
  r.stop();  // run() exits once all tasks are done
  r.run();
  ASSERT_TRUE(order.size() == 3);
  EXPECT_EQ(order[0], 100);
  EXPECT_EQ(order[1], 200);
  EXPECT_EQ(order[2], 300);
}

TEST(Timer, WithTimeoutTaskWins) {
  FakeRing ring;
  FakeClock clk;
  Reactor::Options opts;
  opts.clock = [&clk] { return clk.now; };
  Reactor r(ring, opts);
  ring.on_wait = [&clk](std::optional<std::chrono::nanoseconds> t) {
    if (t && t->count() > 0) clk.now += *t;
  };

  bool got = false;
  spawn(
      [](bool* g) -> Task<void> {
        auto v = co_await with_timeout(
            []() -> Task<int> {
              co_await sleep_for(10ms);
              co_return 7;
            }(),
            1s);
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, 7);
        *g = true;
      }(&got),
      r);
  r.stop();
  r.run();
  EXPECT_TRUE(got);
}

TEST(Timer, WithTimeoutTimerWins) {
  FakeRing ring;
  FakeClock clk;
  Reactor::Options opts;
  opts.clock = [&clk] { return clk.now; };
  Reactor r(ring, opts);
  ring.on_wait = [&clk](std::optional<std::chrono::nanoseconds> t) {
    if (t && t->count() > 0) clk.now += *t;
  };

  bool got = false;
  bool slow_done = false;
  spawn(
      [](bool* g, bool* sd) -> Task<void> {
        auto v = co_await with_timeout(
            [](bool* sdd) -> Task<int> {
              co_await sleep_for(5s);
              *sdd = true;  // keeps running after the timeout (cooperative model)
              co_return 7;
            }(sd),
            50ms);
        EXPECT_FALSE(v.has_value());
        *g = true;
      }(&got, &slow_done),
      r);
  r.stop();
  r.run();  // run drains the still-sleeping detached task too
  EXPECT_TRUE(got);
  EXPECT_TRUE(slow_done);  // discarded, but it did finish
}

// Token bucket (plan doc 10 §4.3) on the fake clock: unconfigured acquires are free,
// a drained bucket parks the coroutine for exactly the refill time, and debt-mode
// lets over-burst requests through while charging the following acquire.
TEST(TokenBucket, ShapesToConfiguredRate) {
  FakeRing ring;
  FakeClock clk;
  Reactor::Options opts;
  opts.clock = [&clk] { return clk.now; };
  Reactor r(ring, opts);
  ring.on_wait = [&clk](std::optional<std::chrono::nanoseconds> t) {
    if (t && t->count() > 0) clk.now += *t;
  };

  TokenBucket tb;
  const TimePoint start = clk.now;
  TimePoint after_free{}, after_shaped{};
  spawn(
      [](TokenBucket* tb, FakeClock* clk, TimePoint* free_t,
         TimePoint* shaped_t) -> Task<void> {
        co_await tb->acquire(1u << 20);  // unconfigured: immediate
        *free_t = clk->now;
        tb->configure(1000);        // 1000 tokens/s, burst = 1000, starts full
        co_await tb->acquire(1000); // drains the bucket, still immediate
        co_await tb->acquire(500);  // must wait ~0.5s of (fake) refill
        *shaped_t = clk->now;
      }(&tb, &clk, &after_free, &after_shaped),
      r);
  r.stop();
  r.run();
  EXPECT_TRUE(after_free == start);
  auto waited =
      std::chrono::duration_cast<std::chrono::milliseconds>(after_shaped - start).count();
  EXPECT_TRUE(waited >= 500);
  EXPECT_TRUE(waited < 1000);
}

TEST(TokenBucket, DebtModePassesOverBurstRequests) {
  FakeRing ring;
  FakeClock clk;
  Reactor::Options opts;
  opts.clock = [&clk] { return clk.now; };
  Reactor r(ring, opts);
  ring.on_wait = [&clk](std::optional<std::chrono::nanoseconds> t) {
    if (t && t->count() > 0) clk.now += *t;
  };

  TokenBucket tb;
  tb.configure(1000);  // burst 1000
  const TimePoint start = clk.now;
  TimePoint after_big{}, after_small{};
  spawn(
      [](TokenBucket* tb, FakeClock* clk, TimePoint* big_t,
         TimePoint* small_t) -> Task<void> {
        co_await tb->acquire(5000);  // > burst: passes at once, leaves a 4000 debt
        *big_t = clk->now;
        co_await tb->acquire(1);     // repays the debt: ~4s of refill
        *small_t = clk->now;
        tb->configure(0);            // disabling releases everything instantly
        co_await tb->acquire(1u << 30);
      }(&tb, &clk, &after_big, &after_small),
      r);
  r.stop();
  r.run();
  EXPECT_TRUE(after_big == start);
  auto repaid =
      std::chrono::duration_cast<std::chrono::milliseconds>(after_small - start).count();
  EXPECT_TRUE(repaid >= 4000);
  EXPECT_TRUE(repaid < 5000);
}
