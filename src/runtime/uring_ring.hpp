#pragma once
// io_uring implementation of RingOps (design 02 §2.3, decision D1). Classic proactor:
// every CQE carries the OpHandle* of the waiting coroutine. Cross-thread wake() is an
// eventfd with a permanently re-armed read op.

#include <liburing.h>

#include <memory>
#include <vector>

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
  // Flushes queued SQEs; a negative return leaves them in the SQ ring for the next
  // attempt (counted + throttled-logged, plan doc 10 §1.4).
  int submit_all();
  // Consumes ready CQEs into `out`, filtering wake/timeout entries; a consumed wake
  // sets wake_rearm_pending_ (re-armed at the top of the next wait()).
  size_t reap_ready(std::span<Completion> out);
  // get_sqe() backpressure relief: moves ready CQEs into backlog_ so the CQ frees up
  // without dropping completions; wait() serves the backlog first.
  void park_ready();

  io_uring ring_{};
  bool ring_init_ = false;
  int evfd_ = -1;
  uint64_t wake_buf_ = 0;
  // Tag address used as user_data for the internal eventfd read op.
  char wake_tag_ = 0;
  bool wake_rearm_pending_ = false;
  std::vector<Completion> backlog_;
  size_t backlog_head_ = 0;
  uint64_t submit_failures_ = 0;
};

}  // namespace lnfs::rt
