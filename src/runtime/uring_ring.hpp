#pragma once
// io_uring implementation of RingOps (design 02 §2.3, decision D1). Classic proactor:
// every CQE carries the OpHandle* of the waiting coroutine. Cross-thread wake() is an
// eventfd with a permanently re-armed read op.

#include <liburing.h>

#include <memory>

#include "runtime/ring_ops.hpp"
#include "util/result.hpp"

namespace lnfs::rt {

class UringRing final : public RingOps {
 public:
  static Result<std::unique_ptr<UringRing>> create(unsigned entries = 1024);
  ~UringRing() override;

  void prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) override;
  void prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) override;
  void prep_fsync(OpHandle* op, int fd, bool datasync) override;
  void prep_recv(OpHandle* op, int fd, std::span<std::byte> buf) override;
  void prep_sendv(OpHandle* op, int fd, const iovec* iov, int iovcnt) override;
  void prep_accept(OpHandle* op, int fd, sockaddr* addr, socklen_t* alen) override;
  void prep_statx(OpHandle* op, int dirfd, const char* path, int flags, unsigned mask,
                  struct statx* out) override;
  void prep_openat(OpHandle* op, int dirfd, const char* path, int flags, mode_t mode) override;
  void prep_close(OpHandle* op, int fd) override;
  void prep_cancel_fd(OpHandle* op, int fd) override;

  size_t wait(std::span<Completion> out, std::optional<std::chrono::nanoseconds> timeout) override;
  void wake() override;

 private:
  UringRing() = default;
  io_uring_sqe* get_sqe();
  void arm_wake();

  io_uring ring_{};
  bool ring_init_ = false;
  int evfd_ = -1;
  uint64_t wake_buf_ = 0;
  // Tag address used as user_data for the internal eventfd read op.
  char wake_tag_ = 0;
};

}  // namespace lnfs::rt
