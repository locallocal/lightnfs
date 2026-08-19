// Fault-injection semantics through FakeRing: EINTR surfacing, short reads, cancel-by-fd.

#include "mini_test.hpp"
#include "runtime/io.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"

using namespace lnfs::rt;
using testing::FakeRing;

TEST(FakeRing, ShortReadThenRetryLoop) {
  FakeRing ring;
  Reactor r(ring);
  std::string collected;
  spawn(
      [](std::string* out) -> Task<void> {
        std::byte buf[8];
        // read-until-n loop like RecordStream's: handles short reads and EINTR
        size_t need = 8;
        while (need > 0) {
          int n = co_await uring_recv(5, std::span<std::byte>(buf + (8 - need), need));
          if (n == -EINTR) continue;
          if (n <= 0) break;
          out->append(reinterpret_cast<char*>(buf) + (8 - need), static_cast<size_t>(n));
          need -= static_cast<size_t>(n);
        }
      }(&collected),
      r);

  while (r.poll_once()) {
  }
  ring.complete_with_data(ring.take(FakeRing::Kind::kRecv, 5), std::string_view("abc"));
  while (r.poll_once()) {
  }
  ring.complete(ring.take(FakeRing::Kind::kRecv, 5), -EINTR);  // injected EINTR
  while (r.poll_once()) {
  }
  ring.complete_with_data(ring.take(FakeRing::Kind::kRecv, 5), std::string_view("de"));
  while (r.poll_once()) {
  }
  ring.complete_with_data(ring.take(FakeRing::Kind::kRecv, 5), std::string_view("fgh"));
  while (r.poll_once()) {
  }
  EXPECT_STREQ(collected, "abcdefgh");
}

TEST(FakeRing, CancelFd) {
  FakeRing ring;
  Reactor r(ring);
  int read_res = 0;
  int cancel_res = 0;
  spawn(
      [](int* out) -> Task<void> {
        std::byte b[4];
        *out = co_await uring_recv(11, std::span<std::byte>(b, 4));
      }(&read_res),
      r);
  while (r.poll_once()) {
  }
  spawn(
      [](int* out) -> Task<void> { *out = co_await uring_cancel_fd(11); }(&cancel_res),
      r);
  while (r.poll_once()) {
  }
  EXPECT_EQ(read_res, -ECANCELED);
  EXPECT_EQ(cancel_res, 1);
}

TEST(FakeRing, CompletionReordering) {
  // Two ops on different "fds" complete in reverse submit order: xid-independent replies.
  FakeRing ring;
  Reactor r(ring);
  std::vector<int> done;
  for (int fd : {1, 2}) {
    spawn(
        [](int fd_, std::vector<int>* d) -> Task<void> {
          co_await uring_fsync(fd_, false);
          d->push_back(fd_);
        }(fd, &done),
        r);
  }
  while (r.poll_once()) {
  }
  ring.complete(ring.take(FakeRing::Kind::kFsync, 2), 0);  // 2 first
  ring.complete(ring.take(FakeRing::Kind::kFsync, 1), 0);
  while (r.poll_once()) {
  }
  ASSERT_TRUE(done.size() == 2);
  EXPECT_EQ(done[0], 2);
  EXPECT_EQ(done[1], 1);
}
