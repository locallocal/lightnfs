#include "runtime/uring_ring.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
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

}  // namespace

Result<std::unique_ptr<UringRing>> UringRing::create(unsigned entries) {
  auto r = std::unique_ptr<UringRing>(new UringRing());
  int rc = io_uring_queue_init(entries, &r->ring_, 0);
  if (rc < 0) return Err(errno_from_neg(rc));
  r->ring_init_ = true;
  if (auto missing = missing_opcodes(&r->ring_); !missing.empty()) {
    LNFS_WARN("io_uring lacks required opcodes: {} (kernel too old for the uring ring)",
              missing);
    return Err(errno_from(ENOSYS));
  }
  r->evfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (r->evfd_ < 0) return Err(errno_from(errno));
  r->arm_wake();
  io_uring_submit(&r->ring_);
  return r;
}

UringRing::~UringRing() {
  if (ring_init_) io_uring_queue_exit(&ring_);
  if (evfd_ >= 0) close(evfd_);
}

io_uring_sqe* UringRing::get_sqe() {
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    io_uring_submit(&ring_);
    sqe = io_uring_get_sqe(&ring_);
  }
  assert(sqe && "SQ ring exhausted even after submit");
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
void UringRing::prep_accept(OpHandle* op, int fd, sockaddr* addr, socklen_t* alen) {
  auto* sqe = get_sqe();
  io_uring_prep_accept(sqe, fd, addr, alen, SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, op);
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
  auto* sqe = get_sqe();
  io_uring_prep_close(sqe, fd);
  io_uring_sqe_set_data(sqe, op);
}
void UringRing::prep_cancel_fd(OpHandle* op, int fd) {
  auto* sqe = get_sqe();
  io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
  io_uring_sqe_set_data(sqe, op);
}

size_t UringRing::wait(std::span<Completion> out,
                       std::optional<std::chrono::nanoseconds> timeout) {
  io_uring_submit(&ring_);

  if (io_uring_cq_ready(&ring_) == 0) {
    if (timeout && timeout->count() == 0) {
      // pure poll
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
      arm_wake();  // submitted on the next wait()
      continue;
    }
    out[n++] = Completion{static_cast<OpHandle*>(p), cqe->res};
    ++consumed;
  }
  io_uring_cq_advance(&ring_, consumed);
  return n;
}

}  // namespace lnfs::rt
