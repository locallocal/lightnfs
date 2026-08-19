// Task<T> semantics on a reactor driven by FakeRing: laziness, chaining, spawn bookkeeping,
// io awaiter completion, exception fail-fast is NOT tested here (it aborts by design).

#include "mini_test.hpp"
#include "runtime/io.hpp"
#include "runtime/reactor.hpp"
#include "runtime/task.hpp"
#include "runtime/testing/fake_ring.hpp"

using namespace lnfs::rt;
using testing::FakeRing;

namespace {

Task<int> forty_two(bool* started) {
  *started = true;
  co_return 42;
}

Task<int> add_one(Task<int> inner) {
  int v = co_await std::move(inner);
  co_return v + 1;
}

}  // namespace

TEST(Task, LazyStart) {
  FakeRing ring;
  Reactor r(ring);
  bool started = false;
  bool done = false;
  auto t = forty_two(&started);
  EXPECT_FALSE(started);  // lazy: nothing ran yet
  spawn(
      [](Task<int> inner, bool* done_, bool* started_) -> Task<void> {
        int v = co_await add_one(std::move(inner));
        EXPECT_EQ(v, 43);
        EXPECT_TRUE(*started_);
        *done_ = true;
      }(std::move(t), &done, &started),
      r);
  EXPECT_FALSE(done);  // spawn posts; runs only when the reactor turns
  while (r.poll_once()) {
  }
  EXPECT_TRUE(done);
  EXPECT_EQ(r.live_tasks(), 0);
}

TEST(Task, IoAwaiterRoundTrip) {
  FakeRing ring;
  Reactor r(ring);
  std::byte buf[16];
  int got = -1;
  spawn(
      [](std::byte* b, int* out) -> Task<void> {
        int res = co_await uring_read(7, std::span<std::byte>(b, 16), 0);
        *out = res;
      }(buf, &got),
      r);
  while (r.poll_once()) {
  }
  // Suspended on the read now.
  EXPECT_TRUE(ring.has_pending(FakeRing::Kind::kRead, 7));
  auto op = ring.take(FakeRing::Kind::kRead, 7);
  ring.complete_with_data(op, std::string_view("hello"));
  while (r.poll_once()) {
  }
  EXPECT_EQ(got, 5);
  EXPECT_EQ((char)buf[0], 'h');
  EXPECT_EQ(r.live_tasks(), 0);
}

TEST(Task, NegativeErrnoSurfaces) {
  FakeRing ring;
  Reactor r(ring);
  int got = 0;
  spawn(
      [](int* out) -> Task<void> {
        std::byte b[4];
        *out = co_await uring_read(9, std::span<std::byte>(b, 4), 0);
      }(&got),
      r);
  while (r.poll_once()) {
  }
  ring.complete(ring.take(FakeRing::Kind::kRead, 9), -EIO);
  while (r.poll_once()) {
  }
  EXPECT_EQ(got, -EIO);
}

TEST(Task, ManySequentialOps) {
  // A chain of 1000 io ops through one coroutine: exercises symmetric transfer + frame reuse.
  FakeRing ring;
  Reactor r(ring);
  int count = 0;
  spawn(
      [](int* c) -> Task<void> {
        std::byte b[1];
        for (int i = 0; i < 1000; ++i) {
          co_await uring_fsync(3, false);
          ++*c;
        }
        (void)b;
      }(&count),
      r);
  for (int i = 0; i < 1000; ++i) {
    while (r.poll_once()) {
    }
    ring.complete(ring.take(FakeRing::Kind::kFsync, 3), 0);
  }
  while (r.poll_once()) {
  }
  EXPECT_EQ(count, 1000);
}
