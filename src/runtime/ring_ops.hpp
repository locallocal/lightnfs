#pragma once
// RingOps: the seam between Reactor and the kernel (design 02 §2.8): the reactor and all io
// wrappers talk only to this interface, so unit tests inject a FakeRing (scripted completions,
// EINTR/short-read injection) and production picks io_uring or the epoll fallback.
//
// Contract:
//  - prep_* may only be called from the reactor thread; ops are submitted in the next wait().
//  - every prep'd op eventually produces exactly one Completion whose res follows the io_uring
//    convention: >=0 success value, <0 negated errno.
//  - wait() blocks up to `timeout` (nullopt = until woken) and fills `out`.
//  - wake() is thread-safe and interrupts a blocked wait().

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

struct statx;  // <sys/stat.h> may not pull it on old glibc; linux/stat.h does

namespace lnfs::rt {

// One in-flight operation; lives in the awaiting coroutine's frame.
struct OpHandle {
  std::coroutine_handle<> waiter;
  int32_t res = 0;
};

struct Completion {
  OpHandle* op;
  int32_t res;
};

class RingOps {
 public:
  virtual ~RingOps() = default;

  virtual void prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) = 0;
  virtual void prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) = 0;
  virtual void prep_fsync(OpHandle* op, int fd, bool datasync) = 0;
  virtual void prep_recv(OpHandle* op, int fd, std::span<std::byte> buf) = 0;
  virtual void prep_sendv(OpHandle* op, int fd, const iovec* iov, int iovcnt) = 0;
  virtual void prep_accept(OpHandle* op, int fd, sockaddr* addr, socklen_t* alen) = 0;
  virtual void prep_statx(OpHandle* op, int dirfd, const char* path, int flags, unsigned mask,
                          struct statx* out) = 0;
  virtual void prep_openat(OpHandle* op, int dirfd, const char* path, int flags, mode_t mode) = 0;
  virtual void prep_close(OpHandle* op, int fd) = 0;
  // Best-effort cancel of all in-flight ops on fd; each cancelled op completes with -ECANCELED.
  virtual void prep_cancel_fd(OpHandle* op, int fd) = 0;

  virtual size_t wait(std::span<Completion> out,
                      std::optional<std::chrono::nanoseconds> timeout) = 0;
  virtual void wake() = 0;
};

}  // namespace lnfs::rt
