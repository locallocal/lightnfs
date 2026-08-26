// Real-kernel smoke tests for both RingOps backends: file IO (openat/statx/write/fsync/read/
// close) and socketpair recv/sendv. The identical scenario runs on UringRing (skipped if the
// kernel/sandbox denies io_uring) and EpollRing.

#include <fcntl.h>
#include <linux/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <thread>

#include "mini_test.hpp"
#include "runtime/epoll_ring.hpp"
#include "runtime/io.hpp"
#include "runtime/reactor.hpp"
#include "runtime/uring_ring.hpp"

using namespace lnfs;
using namespace lnfs::rt;

namespace {

void run_file_io_scenario(RingOps& ring) {
  Reactor r(ring);
  char tmpl[] = "/tmp/lnfs_smoke_XXXXXX";
  int dirfd_unused = mkstemp(tmpl);
  close(dirfd_unused);
  unlink(tmpl);
  std::string path = tmpl;

  bool ok = false;
  spawn(
      [](std::string p, bool* okk, Reactor* rr) -> Task<void> {
        int fd = co_await uring_openat(AT_FDCWD, p.c_str(), O_CREAT | O_RDWR, 0600);
        EXPECT_TRUE(fd >= 0);
        const char msg[] = "phase-zero";
        int w = co_await uring_write(
            fd, std::span<const std::byte>((const std::byte*)msg, 10), 0);
        EXPECT_EQ(w, 10);
        EXPECT_EQ(co_await uring_fsync(fd, true), 0);

        struct statx st{};
        int sr = co_await uring_statx(AT_FDCWD, p.c_str(), 0, STATX_SIZE, &st);
        EXPECT_EQ(sr, 0);
        EXPECT_EQ(st.stx_size, 10u);

        std::byte buf[16];
        int n = co_await uring_read(fd, std::span<std::byte>(buf, 16), 0);
        EXPECT_EQ(n, 10);
        EXPECT_TRUE(std::memcmp(buf, msg, 10) == 0);

        // negative-errno convention on a bad fd
        int bad = co_await uring_read(-1, std::span<std::byte>(buf, 16), 0);
        EXPECT_EQ(bad, -EBADF);

        EXPECT_EQ(co_await uring_close(fd), 0);
        *okk = true;
        rr->stop();
      }(path, &ok, &r),
      r);
  r.run();
  unlink(tmpl);
  EXPECT_TRUE(ok);
}

void run_socket_scenario(RingOps& ring) {
  Reactor r(ring);
  int sv[2];
  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  std::thread peer([fd = sv[1]] {
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    // echo back
    (void)!write(fd, buf, n);
    close(fd);
  });

  bool ok = false;
  spawn(
      [](int fd, bool* okk, Reactor* rr) -> Task<void> {
        const char msg[] = "ping-pong";
        iovec iov{const_cast<char*>(msg), 9};
        int w = co_await uring_sendv(fd, &iov, 1);
        EXPECT_EQ(w, 9);
        std::byte buf[64];
        int n = co_await uring_recv(fd, std::span<std::byte>(buf, 64));
        EXPECT_EQ(n, 9);
        EXPECT_TRUE(std::memcmp(buf, msg, 9) == 0);
        // peer closed: next recv returns 0 (EOF)
        int e = co_await uring_recv(fd, std::span<std::byte>(buf, 64));
        EXPECT_EQ(e, 0);
        *okk = true;
        rr->stop();
      }(sv[0], &ok, &r),
      r);
  r.run();
  peer.join();
  close(sv[0]);
  EXPECT_TRUE(ok);
}

bool uring_available(std::unique_ptr<UringRing>& out) {
  auto r = UringRing::create(64);
  if (!r) {
    std::printf("  (io_uring unavailable: errno=%d — skipped)\n", raw(r.error()));
    return false;
  }
  out = std::move(*r);
  return true;
}

}  // namespace

TEST(UringSmoke, FileIo) {
  std::unique_ptr<UringRing> ring;
  if (!uring_available(ring)) return;
  run_file_io_scenario(*ring);
}

TEST(UringSmoke, SocketPair) {
  std::unique_ptr<UringRing> ring;
  if (!uring_available(ring)) return;
  run_socket_scenario(*ring);
}

// Plan doc 10 §1.4: SQ exhaustion and CQ-overflow backpressure must neither crash nor
// lose ops. A 4-entry ring gets 256 immediate /dev/null writes prepped back-to-back
// with no intervening wait(): get_sqe() has to recycle SQ slots by flushing, ride out
// -EBUSY by parking ready completions, and every op must still complete exactly once.
TEST(UringSmoke, SqExhaustionAndCqOverflow) {
  auto made = UringRing::create(4);
  if (!made) {
    std::printf("  (io_uring unavailable: errno=%d — skipped)\n", raw(made.error()));
    return;
  }
  auto& ring = **made;
  int fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  ASSERT_TRUE(fd >= 0);
  constexpr size_t kOps = 256;
  std::vector<OpHandle> ops(kOps);
  const char byte = 'x';
  std::span<const std::byte> one(reinterpret_cast<const std::byte*>(&byte), 1);
  for (auto& op : ops) ring.prep_write(&op, fd, one, 0);

  std::vector<int> seen(kOps, 0);
  size_t total = 0;
  Completion comps[32];
  for (int round = 0; total < kOps && round < 1000; ++round) {
    size_t n = ring.wait(std::span<Completion>(comps),
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::milliseconds(10)));
    for (size_t i = 0; i < n; ++i) {
      EXPECT_EQ(comps[i].res, 1);
      size_t idx = static_cast<size_t>(comps[i].op - ops.data());
      ASSERT_TRUE(idx < kOps);
      ++seen[idx];
      ++total;
    }
  }
  EXPECT_EQ(total, kOps);
  for (size_t i = 0; i < kOps; ++i) EXPECT_EQ(seen[i], 1);
  close(fd);
}

TEST(EpollSmoke, FileIo) {
  EpollRing ring(2);
  run_file_io_scenario(ring);
}

TEST(EpollSmoke, SocketPair) {
  EpollRing ring(2);
  run_socket_scenario(ring);
}
