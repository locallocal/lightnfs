#pragma once
// io_uring implementation of RingOps (design 02 §2.3, decision D1). Classic proactor:
// every CQE carries the OpHandle* of the waiting coroutine. Cross-thread wake() is an
// eventfd with a permanently re-armed read op.
//
// Modern setup (plan doc 10 §2.3): one-reactor-one-ring-one-thread is the textbook
// SINGLE_ISSUER | DEFER_TASKRUN case, so create() probes a flag ladder
// (SINGLE_ISSUER+DEFER_TASKRUN → SINGLE_ISSUER+COOP_TASKRUN → COOP_TASKRUN → 0) and
// sizes the CQ independently of the SQ. Rings needing an issuer are created disabled
// and enabled by bind_submitter() on the reactor thread. Accept on a 5.19+ kernel is
// multishot: one standing SQE per listener feeds every incoming connection.

#include <liburing.h>

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "runtime/ring_ops.hpp"
#include "util/result.hpp"

namespace lnfs::rt {

class UringRing final : public RingOps {
 public:
  struct Setup {
    unsigned sq_entries = 1024;
    unsigned cq_entries = 0;  // 0 = 8 × sq_entries (decoupled from SQ, plan §2.3)
    bool sqpoll = false;      // kernel-thread submission (design 02 §2.63)
    unsigned sqpoll_idle_ms = 1000;
  };
  static Result<std::unique_ptr<UringRing>> create(unsigned entries = 1024);
  static Result<std::unique_ptr<UringRing>> create(const Setup& setup);
  ~UringRing() override;

  void bind_submitter() override;

  void prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) override;
  void prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) override;
  void prep_writev(OpHandle* op, int fd, const iovec* iov, int iovcnt, uint64_t off) override;
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

  // The setup flags the kernel accepted (logged by Runtime; tests inspect them).
  unsigned setup_flags() const { return setup_flags_; }
  bool multishot_accept() const { return multishot_accept_; }

 private:
  // Standing multishot accept per listening fd. Accepted fds the reactor has not yet
  // asked for wait in `queued`; a waiting accept parks its OpHandle in `waiter`.
  struct AcceptStream {
    int fd = -1;
    bool armed = false;
    OpHandle* waiter = nullptr;
    int32_t pending_err = 0;  // terminal error to hand to the next accept
    std::deque<int> queued;
  };

  UringRing() = default;
  io_uring_sqe* get_sqe();
  void arm_wake();
  void arm_multishot(int fd, AcceptStream& s);
  void purge_stream(int fd);  // close queued fds, drop the stream
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
  bool enabled_ = false;        // bind_submitter() ran (or the ring never was disabled)
  bool needs_enable_ = false;   // created with IORING_SETUP_R_DISABLED
  unsigned setup_flags_ = 0;
  bool multishot_accept_ = false;
  int evfd_ = -1;
  bool wake_armed_ = false;
  uint64_t wake_buf_ = 0;
  // Tag address used as user_data for the internal eventfd read op. Aligned so it can
  // never collide with the low-bit-tagged AcceptStream user_data values.
  alignas(8) char wake_tag_ = 0;
  bool wake_rearm_pending_ = false;
  std::vector<Completion> backlog_;
  size_t backlog_head_ = 0;
  uint64_t submit_failures_ = 0;
  std::unordered_map<int, AcceptStream> accept_streams_;
  std::vector<int> rearm_pending_;  // streams to re-arm before the next blocking wait
  std::vector<int> purge_pending_;  // streams to drop once the CQE batch is consumed
};

}  // namespace lnfs::rt
