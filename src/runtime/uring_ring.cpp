#include "runtime/uring_ring.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>

#include "util/log.hpp"

namespace lnfs::rt {

namespace {

// Every opcode this ring issues (kernel-differences risk, design 02 §2.3 / plan §10):
// probed once at ring creation so an old kernel degrades to the epoll fallback at
// startup instead of failing per-op with EINVAL mid-request.
constexpr std::pair<int, const char*> kRequiredOps[] = {
    {IORING_OP_READ, "read"},     {IORING_OP_WRITE, "write"},
    {IORING_OP_WRITEV, "writev"}, {IORING_OP_RECV, "recv"},
    {IORING_OP_ACCEPT, "accept"}, {IORING_OP_OPENAT, "openat"},
    {IORING_OP_CLOSE, "close"},   {IORING_OP_FSYNC, "fsync"},
    {IORING_OP_STATX, "statx"},   {IORING_OP_ASYNC_CANCEL, "cancel"},
};

// Empty string = all supported. A null probe (pre-5.6 kernel, no IORING_REGISTER_PROBE)
// reports everything missing — those kernels predate IORING_OP_READ/STATX anyway.
std::string missing_opcodes(io_uring* ring) {
  io_uring_probe* p = io_uring_get_probe_ring(ring);
  std::string missing;
  for (auto [op, name] : kRequiredOps) {
    if (p && io_uring_opcode_supported(p, op)) continue;
    if (!missing.empty()) missing += ',';
    missing += name;
  }
  if (p) io_uring_free_probe(p);
  return missing;
}

// Multishot accept landed in 5.19; there is no direct feature bit, so probe an opcode
// introduced in the same release (IORING_OP_SOCKET — an enum, so the header guard is
// the 5.19-era IORING_SETUP_COOP_TASKRUN macro).
bool kernel_has_multishot_accept(io_uring* ring) {
#ifdef IORING_SETUP_COOP_TASKRUN
  io_uring_probe* p = io_uring_get_probe_ring(ring);
  bool ok = p && io_uring_opcode_supported(p, IORING_OP_SOCKET);
  if (p) io_uring_free_probe(p);
  return ok;
#else
  (void)ring;
  return false;
#endif
}

const char* flags_name(unsigned flags) {
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
  if (flags & IORING_SETUP_DEFER_TASKRUN) return "single_issuer+defer_taskrun";
#endif
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_COOP_TASKRUN)
  if (flags & IORING_SETUP_SINGLE_ISSUER) return "single_issuer+coop_taskrun";
#endif
#ifdef IORING_SETUP_COOP_TASKRUN
  if (flags & IORING_SETUP_COOP_TASKRUN) return "coop_taskrun";
#endif
  if (flags & IORING_SETUP_SQPOLL) return "sqpoll";
  return "plain";
}

}  // namespace

Result<std::unique_ptr<UringRing>> UringRing::create(unsigned entries) {
  return create(Setup{.sq_entries = entries});
}

Result<std::unique_ptr<UringRing>> UringRing::create(const Setup& setup) {
  auto r = std::unique_ptr<UringRing>(new UringRing());

  // Setup ladder (plan doc 10 §2.3): the reactor model is exactly one submitter thread
  // per ring, so ask for the strongest single-issuer mode the kernel offers and step
  // down on EINVAL. SINGLE_ISSUER rings are created disabled because creation happens
  // on the setup thread while the reactor thread is the issuer (bind_submitter()).
  struct Candidate {
    unsigned flags;
    bool needs_enable;
  };
  Candidate ladder[5];
  int ladder_n = 0;
  if (setup.sqpoll) {
    // SQPOLL (design 02 §2.63): mutually exclusive with DEFER_TASKRUN; the sq thread
    // is the issuer, so no enable dance either.
    ladder[ladder_n++] = {IORING_SETUP_SQPOLL, false};
  }
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
  ladder[ladder_n++] = {IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                            IORING_SETUP_R_DISABLED,
                        true};
#endif
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_COOP_TASKRUN)
  ladder[ladder_n++] = {IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
                            IORING_SETUP_R_DISABLED,
                        true};
#endif
#ifdef IORING_SETUP_COOP_TASKRUN
  ladder[ladder_n++] = {IORING_SETUP_COOP_TASKRUN, false};
#endif
  ladder[ladder_n++] = {0, false};

  // CQ degradation: the ring memory can be memlock-charged, so a constrained
  // RLIMIT_MEMLOCK must cost CQ headroom, not the whole uring backend.
  unsigned cq_target = setup.cq_entries ? setup.cq_entries : setup.sq_entries * 8;
  unsigned cq_entries = cq_target;
  int rc = -EINVAL;
  for (;;) {
    for (int i = 0; i < ladder_n; ++i) {
      io_uring_params params{};
      params.flags = ladder[i].flags | IORING_SETUP_CQSIZE;
      params.cq_entries = cq_entries;
      if (ladder[i].flags & IORING_SETUP_SQPOLL)
        params.sq_thread_idle = setup.sqpoll_idle_ms;
      rc = io_uring_queue_init_params(setup.sq_entries, &r->ring_, &params);
      if (rc == 0) {
        r->setup_flags_ = ladder[i].flags;
        r->needs_enable_ = ladder[i].needs_enable;
        break;
      }
      if (ladder[i].flags & IORING_SETUP_SQPOLL)
        LNFS_WARN("io_uring SQPOLL requested but unavailable (errno={}); falling back",
                  -rc);
    }
    if (rc == 0 || rc != -ENOMEM || cq_entries <= setup.sq_entries * 2) break;
    cq_entries /= 2;
  }
  if (rc < 0) return Err(errno_from_neg(rc));
  if (cq_entries != cq_target)
    LNFS_WARN("io_uring CQ reduced {} -> {} (memlock/memory pressure)", cq_target,
              cq_entries);
  r->ring_init_ = true;
  r->enabled_ = !r->needs_enable_;
  if (auto missing = missing_opcodes(&r->ring_); !missing.empty()) {
    LNFS_WARN("io_uring lacks required opcodes: {} (kernel too old for the uring ring)",
              missing);
    return Err(errno_from(ENOSYS));
  }
  r->multishot_accept_ = kernel_has_multishot_accept(&r->ring_);
  r->evfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (r->evfd_ < 0) return Err(errno_from(errno));
  LNFS_DEBUG("uring setup: mode={} sq={} cq={} multishot_accept={}",
             flags_name(r->setup_flags_), setup.sq_entries, cq_entries,
             r->multishot_accept_);
  // The wake read is armed by bind_submitter() on the reactor thread: a disabled ring
  // rejects submits, and SINGLE_ISSUER pins submission to that thread anyway.
  return r;
}

void UringRing::bind_submitter() {
  if (enabled_ && wake_armed_) return;
  if (needs_enable_ && !enabled_) {
    int rc = io_uring_enable_rings(&ring_);
    if (rc < 0) {
      LNFS_ERROR("io_uring_enable_rings failed: errno={}", -rc);
      std::abort();
    }
  }
  enabled_ = true;
  if (!wake_armed_) {
    wake_armed_ = true;  // before arm_wake(): it re-enters get_sqe()
    arm_wake();
    submit_all();
  }
}

UringRing::~UringRing() {
  for (auto& [fd, s] : accept_streams_)
    for (int cfd : s.queued) close(cfd);
  if (ring_init_) io_uring_queue_exit(&ring_);
  if (evfd_ >= 0) close(evfd_);
}

int UringRing::submit_all() {
  int rc = io_uring_submit(&ring_);
  if (rc < 0) {
    // -EBUSY is CQ-overflow backpressure: the kernel wants CQ room before accepting
    // more SQEs. The old code dropped this return entirely, so the unsubmitted ops
    // stranded their coroutines forever with no log (plan doc 10 §1.4).
    uint64_t n = ++submit_failures_;
    if ((n & (n - 1)) == 0)
      LNFS_WARN("io_uring_submit failed: errno={} ({} failures total, {} SQEs queued "
                "for retry)",
                -rc, n, io_uring_sq_ready(&ring_));
  }
  return rc;
}

io_uring_sqe* UringRing::get_sqe() {
  // First ring use from the reactor thread claims issuer-ship (prep_* is contractually
  // reactor-thread-only, ring_ops.hpp): a disabled ring would refuse the submits the
  // slot-recycling below relies on.
  if (!enabled_ || !wake_armed_) bind_submitter();
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  // SQ exhausted: flush queued SQEs to recycle slots. Under -EBUSY the flush itself is
  // refused until the CQ drains, so pull kernel-side overflow into the CQ ring and
  // park ready completions in the backlog (served by the next wait()). NDEBUG builds
  // used to hand the null slot straight to io_uring_prep_* (plan doc 10 §1.4).
  for (int attempt = 0; !sqe && attempt < 64; ++attempt) {
    if (submit_all() < 0) {
      (void)io_uring_get_events(&ring_);
      park_ready();
    }
    sqe = io_uring_get_sqe(&ring_);
  }
  if (!sqe) {
    LNFS_ERROR("io_uring SQ still exhausted after 64 submit/park rounds; aborting");
    std::abort();
  }
  return sqe;
}

void UringRing::arm_wake() {
  io_uring_sqe* sqe = get_sqe();
  io_uring_prep_read(sqe, evfd_, &wake_buf_, sizeof(wake_buf_), 0);
  io_uring_sqe_set_data(sqe, &wake_tag_);
}

void UringRing::wake() {
  uint64_t one = 1;
  [[maybe_unused]] ssize_t n = ::write(evfd_, &one, sizeof(one));
}

void UringRing::prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) {
  auto* sqe = get_sqe();
  io_uring_prep_read(sqe, fd, buf.data(), buf.size(), off);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) {
  auto* sqe = get_sqe();
  io_uring_prep_write(sqe, fd, buf.data(), buf.size(), off);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_writev(OpHandle* op, int fd, const iovec* iov, int iovcnt,
                            uint64_t off) {
  auto* sqe = get_sqe();
  io_uring_prep_writev(sqe, fd, iov, iovcnt, off);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_fsync(OpHandle* op, int fd, bool datasync) {
  auto* sqe = get_sqe();
  io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_recv(OpHandle* op, int fd, std::span<std::byte> buf) {
  auto* sqe = get_sqe();
  io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), 0);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_sendv(OpHandle* op, int fd, const iovec* iov, int iovcnt) {
  auto* sqe = get_sqe();
  io_uring_prep_writev(sqe, fd, iov, iovcnt, 0);
  io_uring_sqe_set_data(sqe, op);
}

// ---- accept ----------------------------------------------------------------
// Multishot (plan doc 10 §2.3): one standing SQE per listening fd; the kernel posts a
// CQE per incoming connection with no per-accept submit. Only the addr-less form can
// be multishot (the kernel fills no address); callers that want the peer address
// (ctl/metrics) keep the single-shot path and listeners use getpeername().

void UringRing::arm_multishot(int fd, AcceptStream& s) {
#ifdef IORING_SETUP_COOP_TASKRUN  // liburing >= 2.2: io_uring_prep_multishot_accept
  auto* sqe = get_sqe();
  io_uring_prep_multishot_accept(sqe, fd, nullptr, nullptr, SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&s) | 1));
  s.armed = true;
#else
  (void)fd;
  (void)s;
#endif
}

void UringRing::purge_stream(int fd) {
  auto it = accept_streams_.find(fd);
  if (it == accept_streams_.end()) return;
  for (int cfd : it->second.queued) close(cfd);
  accept_streams_.erase(it);
}

void UringRing::prep_accept(OpHandle* op, int fd, sockaddr* addr, socklen_t* alen) {
  if (!multishot_accept_ || addr != nullptr) {  // single-shot (or old kernel)
    auto* sqe = get_sqe();
    io_uring_prep_accept(sqe, fd, addr, alen, SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, op);
    return;
  }
  AcceptStream& s = accept_streams_[fd];
  s.fd = fd;
  if (!s.queued.empty()) {  // connection already accepted by the standing op
    backlog_.push_back({op, s.queued.front()});
    s.queued.pop_front();
    return;
  }
  if (s.pending_err != 0) {  // terminal error observed with nobody waiting
    backlog_.push_back({op, s.pending_err});
    purge_stream(fd);
    return;
  }
  s.waiter = op;
  if (!s.armed) arm_multishot(fd, s);
}

void UringRing::prep_statx(OpHandle* op, int dirfd, const char* path, int flags, unsigned mask,
                           struct statx* out) {
  auto* sqe = get_sqe();
  io_uring_prep_statx(sqe, dirfd, path, flags, mask, out);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_openat(OpHandle* op, int dirfd, const char* path, int flags, mode_t mode) {
  auto* sqe = get_sqe();
  io_uring_prep_openat(sqe, dirfd, path, flags, mode);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_close(OpHandle* op, int fd) {
  purge_stream(fd);  // a listener being closed: drop its stream before the fd recycles
  auto* sqe = get_sqe();
  io_uring_prep_close(sqe, fd);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_cancel_fd(OpHandle* op, int fd) {
  auto* sqe = get_sqe();
  io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
  io_uring_sqe_set_data(sqe, op);
}

size_t UringRing::reap_ready(std::span<Completion> out) {
  size_t n = 0;
  unsigned head = 0;
  unsigned consumed = 0;
  io_uring_cqe* cqe;
  io_uring_for_each_cqe(&ring_, head, cqe) {
    if (n == out.size()) break;
    uint64_t ud = cqe->user_data;
    if (ud == LIBURING_UDATA_TIMEOUT) {
      ++consumed;
      continue;
    }
    void* p = io_uring_cqe_get_data(cqe);
    if (p == &wake_tag_) {
      ++consumed;
      wake_rearm_pending_ = true;  // prepped at the top of the next wait()
      continue;
    }
    if (ud & 1) {  // multishot accept stream CQE
      auto* s = reinterpret_cast<AcceptStream*>(ud & ~uint64_t(1));
      bool more = false;
#ifdef IORING_CQE_F_MORE
      more = cqe->flags & IORING_CQE_F_MORE;
#endif
      if (!more) s->armed = false;
      if (cqe->res >= 0) {
        if (s->waiter) {
          out[n++] = Completion{std::exchange(s->waiter, nullptr), cqe->res};
        } else {
          s->queued.push_back(cqe->res);
        }
        if (!more && s->fd >= 0) rearm_pending_.push_back(s->fd);
      } else {
        // Terminal (cancel/teardown or a hard accept error): hand it to the waiter, or
        // park it for the next accept. Purge is deferred past this CQE batch — later
        // entries in it may still reference the stream.
        if (s->waiter) {
          out[n++] = Completion{std::exchange(s->waiter, nullptr), cqe->res};
          if (s->fd >= 0) purge_pending_.push_back(s->fd);
        } else {
          s->pending_err = cqe->res;
        }
      }
      ++consumed;
      continue;
    }
    out[n++] = Completion{static_cast<OpHandle*>(p), cqe->res};
    ++consumed;
  }
  io_uring_cq_advance(&ring_, consumed);
  if (!purge_pending_.empty()) {
    auto fds = std::move(purge_pending_);
    purge_pending_.clear();
    for (int fd : fds) purge_stream(fd);
  }
  return n;
}

void UringRing::park_ready() {
  unsigned ready = io_uring_cq_ready(&ring_);
  if (ready == 0) return;
  size_t base = backlog_.size();
  backlog_.resize(base + ready);
  size_t n = reap_ready(std::span<Completion>(backlog_).subspan(base));
  backlog_.resize(base + n);
}

size_t UringRing::wait(std::span<Completion> out,
                       std::optional<std::chrono::nanoseconds> timeout) {
  bind_submitter();  // idempotent; first wait() from the reactor thread enables the ring
  // Completions parked by get_sqe() backpressure relief go out first, in order.
  size_t n = 0;
  while (n < out.size() && backlog_head_ < backlog_.size())
    out[n++] = backlog_[backlog_head_++];
  if (backlog_head_ == backlog_.size()) {
    backlog_.clear();
    backlog_head_ = 0;
  }
  if (wake_rearm_pending_) {  // must be armed before this pass may block
    wake_rearm_pending_ = false;
    arm_wake();
  }
  // A multishot accept that ended (CQ pressure) while a waiter is parked must be
  // re-armed before this pass may block.
  if (!rearm_pending_.empty()) {
    auto fds = std::move(rearm_pending_);
    rearm_pending_.clear();
    for (int fd : fds) {
      auto it = accept_streams_.find(fd);
      if (it != accept_streams_.end() && !it->second.armed && it->second.waiter)
        arm_multishot(fd, it->second);
    }
  }
  bool submitted = submit_all() >= 0;

  if (n == 0 && io_uring_cq_ready(&ring_) == 0) {
    if (timeout && timeout->count() == 0) {
#ifdef IORING_SETUP_DEFER_TASKRUN
      // Pure poll: with DEFER_TASKRUN completions sit in task-work until an enter, so
      // flush explicitly — a raw CQ peek would miss them.
      if (setup_flags_ & IORING_SETUP_DEFER_TASKRUN) (void)io_uring_get_events(&ring_);
#endif
    } else {
      __kernel_timespec ts{}, *tsp = nullptr;
      if (timeout) {
        ts.tv_sec = timeout->count() / 1000000000;
        ts.tv_nsec = timeout->count() % 1000000000;
        tsp = &ts;
      }
      io_uring_cqe* cqe = nullptr;
      // -ETIME / -EINTR are normal; completions (if any) are reaped below.
      (void)io_uring_wait_cqes(&ring_, &cqe, 1, tsp, nullptr);
    }
  }

  n += reap_ready(out.subspan(n));
  // A refused submit left SQEs queued; the reap above made CQ room, so retry now
  // instead of stranding those ops until the next prep or pump.
  if (!submitted) submit_all();
  return n;
}

}  // namespace lnfs::rt
