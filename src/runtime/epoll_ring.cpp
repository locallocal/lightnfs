#include "runtime/epoll_ring.hpp"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>

namespace lnfs::rt {

EpollRing::EpollRing(int file_workers) {
  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  evfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  assert(epfd_ >= 0 && evfd_ >= 0);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = evfd_;
  epoll_ctl(epfd_, EPOLL_CTL_ADD, evfd_, &ev);
  workers_.reserve(file_workers);
  for (int i = 0; i < file_workers; ++i) workers_.emplace_back([this] { file_worker(); });
}

EpollRing::~EpollRing() {
  {
    std::lock_guard lk(wmu_);
    stopping_ = true;
  }
  wcv_.notify_all();
  for (auto& t : workers_) t.join();
  if (epfd_ >= 0) close(epfd_);
  if (evfd_ >= 0) close(evfd_);
}

void EpollRing::wake() {
  uint64_t one = 1;
  [[maybe_unused]] ssize_t n = ::write(evfd_, &one, sizeof(one));
}

void EpollRing::submit_file(MoveOnlyFn job) {
  {
    std::lock_guard lk(wmu_);
    wq_.push_back(std::move(job));
  }
  wcv_.notify_one();
}

void EpollRing::file_worker() {
  for (;;) {
    MoveOnlyFn job;
    {
      std::unique_lock lk(wmu_);
      wcv_.wait(lk, [this] { return stopping_ || !wq_.empty(); });
      if (wq_.empty()) return;
      job = std::move(wq_.front());
      wq_.pop_front();
    }
    job();
  }
}

void EpollRing::push_remote(Completion c) {
  {
    std::lock_guard lk(rmu_);
    remote_ready_.push_back(c);
  }
  wake();
}

// ---- file ops: worker pool ------------------------------------------------

void EpollRing::prep_read(OpHandle* op, int fd, std::span<std::byte> buf, uint64_t off) {
  submit_file([this, op, fd, buf, off] {
    ssize_t r;
    do {
      r = pread(fd, buf.data(), buf.size(), static_cast<off_t>(off));
    } while (r < 0 && errno == EINTR);
    push_remote({op, static_cast<int32_t>(r < 0 ? -errno : r)});
  });
}
void EpollRing::prep_write(OpHandle* op, int fd, std::span<const std::byte> buf, uint64_t off) {
  submit_file([this, op, fd, buf, off] {
    ssize_t r;
    do {
      r = pwrite(fd, buf.data(), buf.size(), static_cast<off_t>(off));
    } while (r < 0 && errno == EINTR);
    push_remote({op, static_cast<int32_t>(r < 0 ? -errno : r)});
  });
}
void EpollRing::prep_fsync(OpHandle* op, int fd, bool datasync) {
  submit_file([this, op, fd, datasync] {
    int r = datasync ? fdatasync(fd) : fsync(fd);
    push_remote({op, r < 0 ? -errno : 0});
  });
}
void EpollRing::prep_statx(OpHandle* op, int dirfd, const char* path, int flags, unsigned mask,
                           struct statx* out) {
  submit_file([this, op, dirfd, path, flags, mask, out] {
    int r = statx(dirfd, path, flags, mask, out);
    push_remote({op, r < 0 ? -errno : 0});
  });
}
void EpollRing::prep_openat(OpHandle* op, int dirfd, const char* path, int flags, mode_t mode) {
  submit_file([this, op, dirfd, path, flags, mode] {
    int r = openat(dirfd, path, flags, mode);
    push_remote({op, r < 0 ? -errno : r});
  });
}
void EpollRing::prep_close(OpHandle* op, int fd) {
  submit_file([this, op, fd] {
    int r = close(fd);
    push_remote({op, r < 0 ? -errno : 0});
  });
}

// ---- socket ops: readiness ------------------------------------------------

int EpollRing::try_sock_op(SockOp& s, int fd) {
  ssize_t r;
  switch (s.kind) {
    case SockOp::kRecv:
      do {
        r = recv(fd, s.buf.data(), s.buf.size(), 0);
      } while (r < 0 && errno == EINTR);
      break;
    case SockOp::kSendv:
      do {
        r = writev(fd, s.iov, s.iovcnt);
      } while (r < 0 && errno == EINTR);
      break;
    case SockOp::kAccept:
      do {
        r = accept4(fd, s.addr, s.alen, SOCK_CLOEXEC);
      } while (r < 0 && errno == EINTR);
      break;
  }
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -EAGAIN;
  return static_cast<int>(r < 0 ? -errno : r);
}

void EpollRing::enqueue_sock(int fd, SockOp op) {
  FdQ& q = socks_[fd];
  if (!q.nonblock_set) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    q.nonblock_set = true;
  }
  bool is_out = op.kind == SockOp::kSendv;
  auto& dq = is_out ? q.out : q.in;
  // Preserve per-fd op order: attempt immediately only if nothing queued ahead of us.
  if (dq.empty()) {
    int r = try_sock_op(op, fd);
    if (r != -EAGAIN) {
      ready_.push_back({op.op, r});
      return;
    }
  }
  dq.push_back(op);
  update_interest(fd, q);
}

void EpollRing::update_interest(int fd, FdQ& q) {
  uint32_t want = 0;
  if (!q.in.empty()) want |= EPOLLIN;
  if (!q.out.empty()) want |= EPOLLOUT;
  if (want == q.armed) return;
  epoll_event ev{};
  ev.events = want;
  ev.data.fd = fd;
  if (q.armed == 0 && want != 0) {
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
  } else if (want == 0) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  } else {
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
  }
  q.armed = want;
}

void EpollRing::service_fd(int fd, uint32_t events) {
  auto it = socks_.find(fd);
  if (it == socks_.end()) return;
  FdQ& q = it->second;
  bool err = events & (EPOLLERR | EPOLLHUP);
  if ((events & EPOLLIN) || err) {
    while (!q.in.empty()) {
      int r = try_sock_op(q.in.front(), fd);
      if (r == -EAGAIN) break;
      ready_.push_back({q.in.front().op, r});
      q.in.pop_front();
    }
  }
  if ((events & EPOLLOUT) || err) {
    while (!q.out.empty()) {
      int r = try_sock_op(q.out.front(), fd);
      if (r == -EAGAIN) break;
      ready_.push_back({q.out.front().op, r});
      q.out.pop_front();
    }
  }
  if (q.in.empty() && q.out.empty()) {
    update_interest(fd, q);
    socks_.erase(it);
  } else {
    update_interest(fd, q);
  }
}

void EpollRing::prep_recv(OpHandle* op, int fd, std::span<std::byte> buf) {
  enqueue_sock(fd, SockOp{op, SockOp::kRecv, buf, nullptr, 0, nullptr, nullptr});
}
void EpollRing::prep_sendv(OpHandle* op, int fd, const iovec* iov, int iovcnt) {
  enqueue_sock(fd, SockOp{op, SockOp::kSendv, {}, iov, iovcnt, nullptr, nullptr});
}
void EpollRing::prep_accept(OpHandle* op, int fd, sockaddr* addr, socklen_t* alen) {
  enqueue_sock(fd, SockOp{op, SockOp::kAccept, {}, nullptr, 0, addr, alen});
}

void EpollRing::prep_cancel_fd(OpHandle* op, int fd) {
  int32_t n = 0;
  auto it = socks_.find(fd);
  if (it != socks_.end()) {
    FdQ& q = it->second;
    for (auto& s : q.in) {
      ready_.push_back({s.op, -ECANCELED});
      ++n;
    }
    for (auto& s : q.out) {
      ready_.push_back({s.op, -ECANCELED});
      ++n;
    }
    q.in.clear();
    q.out.clear();
    update_interest(fd, q);
    socks_.erase(it);
  }
  ready_.push_back({op, n == 0 ? -ENOENT : n});
}

size_t EpollRing::wait(std::span<Completion> out,
                       std::optional<std::chrono::nanoseconds> timeout) {
  {
    std::lock_guard lk(rmu_);
    ready_.insert(ready_.end(), remote_ready_.begin(), remote_ready_.end());
    remote_ready_.clear();
  }

  if (ready_.empty()) {
    int ms;
    if (!timeout) ms = -1;
    else if (timeout->count() == 0) ms = 0;
    else ms = static_cast<int>((timeout->count() + 999999) / 1000000);

    epoll_event evs[64];
    int n = epoll_wait(epfd_, evs, 64, ms);
    for (int i = 0; i < n; ++i) {
      if (evs[i].data.fd == evfd_) {
        uint64_t drain;
        while (read(evfd_, &drain, sizeof(drain)) > 0) {
        }
        std::lock_guard lk(rmu_);
        ready_.insert(ready_.end(), remote_ready_.begin(), remote_ready_.end());
        remote_ready_.clear();
      } else {
        service_fd(evs[i].data.fd, evs[i].events);
      }
    }
  }

  size_t n = std::min(out.size(), ready_.size());
  std::copy(ready_.begin(), ready_.begin() + n, out.begin());
  ready_.erase(ready_.begin(), ready_.begin() + n);
  return n;
}

}  // namespace lnfs::rt
