#pragma once
// FakeRing (design 02 §2.8): scripted RingOps for deterministic runtime tests without a real
// kernel ring. Tests inspect `pending`, complete ops in any order, inject EINTR/short reads,
// and drive timers by advancing an injected clock from `on_wait`.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>

#include "runtime/ring_ops.hpp"

namespace lnfs::rt::testing {

class FakeRing final : public RingOps {
 public:
  enum class Kind {
    kRead,
    kWrite,
    kWritev,
    kFsync,
    kRecv,
    kSendv,
    kAccept,
    kStatx,
    kOpenat,
    kClose,
    kCancelFd
  };

  struct Op {
    OpHandle* op;
    Kind kind;
    int fd = -1;
    uint64_t off = 0;
    std::span<std::byte> wbuf{};        // read/recv destination
    std::span<const std::byte> rbuf{};  // write source
    const iovec* iov = nullptr;
    int iovcnt = 0;
    const char* path = nullptr;
  };

  std::deque<Op> pending;
  std::deque<Completion> ready;
  // Called on every wait(); tests advance a fake clock and/or script completions here.
  std::function<void(std::optional<std::chrono::nanoseconds>)> on_wait;

  // ---- test helpers -------------------------------------------------------

  bool has_pending(Kind k, int fd = -1) const {
    for (const auto& o : pending)
      if (o.kind == k && (fd < 0 || o.fd == fd)) return true;
    return false;
  }
  // Pops the oldest pending op of kind k (any fd if fd<0). Aborts if absent.
  Op take(Kind k, int fd = -1) {
    for (auto it = pending.begin(); it != pending.end(); ++it) {
      if (it->kind == k && (fd < 0 || it->fd == fd)) {
        Op o = *it;
        pending.erase(it);
        return o;
      }
    }
    std::fprintf(stderr, "FakeRing: no pending op of kind %d\n", static_cast<int>(k));
    std::abort();
  }
  Op take_next() {
    assert(!pending.empty());
    Op o = pending.front();
    pending.pop_front();
    return o;
  }

  void complete(const Op& o, int32_t res) { ready.push_back({o.op, res}); }
  // Copies data into the op's destination buffer (short read if data smaller).
  void complete_with_data(const Op& o, std::span<const std::byte> data) {
    size_t n = std::min(data.size(), o.wbuf.size());
    std::memcpy(o.wbuf.data(), data.data(), n);
    ready.push_back({o.op, static_cast<int32_t>(n)});
  }
  void complete_with_data(const Op& o, std::string_view s) {
    complete_with_data(o, std::span<const std::byte>(
                              reinterpret_cast<const std::byte*>(s.data()), s.size()));
  }

  // ---- RingOps ------------------------------------------------------------

  void prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) override {
    pending.push_back(Op{op, Kind::kRead, fd, off, buf});
  }
  void prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) override {
    pending.push_back(Op{op, Kind::kWrite, fd, off, {}, buf});
  }
  void prep_writev(OpHandle* op, int fd, const iovec* iov, int iovcnt, uint64_t off) override {
    Op o{op, Kind::kWritev, fd, off};
    o.iov = iov;
    o.iovcnt = iovcnt;
    pending.push_back(o);
  }
  void prep_fsync(OpHandle* op, int fd, bool) override {
    pending.push_back(Op{op, Kind::kFsync, fd});
  }
  void prep_recv(OpHandle* op, int fd, std::span<std::byte> buf) override {
    pending.push_back(Op{op, Kind::kRecv, fd, 0, buf});
  }
  void prep_sendv(OpHandle* op, int fd, const iovec* iov, int iovcnt) override {
    Op o{op, Kind::kSendv, fd};
    o.iov = iov;
    o.iovcnt = iovcnt;
    pending.push_back(o);
  }
  void prep_accept(OpHandle* op, int fd, sockaddr*, socklen_t*) override {
    pending.push_back(Op{op, Kind::kAccept, fd});
  }
  void prep_statx(OpHandle* op, int dirfd, const char* path, int, unsigned,
                  struct statx*) override {
    Op o{op, Kind::kStatx, dirfd};
    o.path = path;
    pending.push_back(o);
  }
  void prep_openat(OpHandle* op, int dirfd, const char* path, int, mode_t) override {
    Op o{op, Kind::kOpenat, dirfd};
    o.path = path;
    pending.push_back(o);
  }
  void prep_close(OpHandle* op, int fd) override {
    pending.push_back(Op{op, Kind::kClose, fd});
  }
  void prep_cancel_fd(OpHandle* op, int fd) override {
    // Mirror ring semantics: cancel all pending ops on fd with -ECANCELED.
    int32_t n = 0;
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->fd == fd && it->kind != Kind::kCancelFd) {
        ready.push_back({it->op, -ECANCELED});
        it = pending.erase(it);
        ++n;
      } else {
        ++it;
      }
    }
    ready.push_back({op, n == 0 ? -ENOENT : n});
  }

  size_t wait(std::span<Completion> out,
              std::optional<std::chrono::nanoseconds> timeout) override {
    if (on_wait) on_wait(timeout);
    if (ready.empty()) {
      if (!timeout) {
        // Blocking forever with nothing scripted: a test bug, fail loudly.
        std::fprintf(stderr,
                     "FakeRing: reactor blocked forever (%zu pending ops, nothing ready)\n",
                     pending.size());
        std::abort();
      }
      return 0;
    }
    size_t n = std::min(out.size(), ready.size());
    for (size_t i = 0; i < n; ++i) {
      out[i] = ready.front();
      ready.pop_front();
    }
    return n;
  }
  void wake() override {}
};

}  // namespace lnfs::rt::testing
