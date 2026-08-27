#pragma once
// epoll fallback RingOps (design 02 §2.3): same proactor interface for old kernels.
// Sockets: readiness (level-triggered epoll) + non-blocking syscalls performed inside wait().
// File ops (read/write/fsync/statx/openat/close): a small internal blocking-worker pool;
// completions come back through an eventfd.

#include <sys/epoll.h>

#include <condition_variable>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/offload_pool.hpp"  // MoveOnlyFn
#include "runtime/ring_ops.hpp"

namespace lnfs::rt {

class EpollRing final : public RingOps {
 public:
  explicit EpollRing(int file_workers = 2);
  ~EpollRing() override;

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

 private:
  struct SockOp {
    OpHandle* op;
    enum Kind { kRecv, kSendv, kAccept } kind;
    std::span<std::byte> buf;
    const iovec* iov = nullptr;
    int iovcnt = 0;
    sockaddr* addr = nullptr;
    socklen_t* alen = nullptr;
  };
  struct FdQ {
    std::deque<SockOp> in, out;
    bool nonblock_set = false;
    uint32_t armed = 0;  // current epoll interest
  };

  void submit_file(MoveOnlyFn job);
  void file_worker();
  void push_remote(Completion c);
  void enqueue_sock(int fd, SockOp op);
  void update_interest(int fd, FdQ& q);
  // Runs queued socket ops that are ready; pushes completions to ready_.
  void service_fd(int fd, uint32_t events);
  static int try_sock_op(SockOp& s, int fd);  // returns res or -EAGAIN

  // Dense fd-indexed table (plan doc 10 §2.6): fds are small and dense, so direct
  // indexing beats a std::map. null slot = no queued ops for that fd.
  FdQ* fdq(int fd) {
    return fd >= 0 && static_cast<size_t>(fd) < socks_.size() ? socks_[fd].get() : nullptr;
  }
  FdQ& fdq_make(int fd) {
    if (static_cast<size_t>(fd) >= socks_.size()) socks_.resize(fd + 1);
    if (!socks_[fd]) socks_[fd] = std::make_unique<FdQ>();
    return *socks_[fd];
  }

  int epfd_ = -1;
  int evfd_ = -1;
  std::vector<std::unique_ptr<FdQ>> socks_;
  std::vector<Completion> ready_;  // reactor-thread completions

  std::mutex rmu_;
  std::vector<Completion> remote_ready_;  // worker completions

  std::mutex wmu_;
  std::condition_variable wcv_;
  std::deque<MoveOnlyFn> wq_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace lnfs::rt
