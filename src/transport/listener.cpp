#include "transport/listener.hpp"

#include "obs/metrics.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "runtime/io.hpp"
#include "util/log.hpp"

namespace lnfs::transport {

using namespace lnfs::rt;

Result<std::unique_ptr<Listener>> Listener::create(uint16_t port, TransportConfig cfg,
                                                   rpc::Dispatcher& disp, rt::Runtime& rt) {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int zero = 0;
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));  // dual-stack

  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      listen(fd, 1024) < 0) {
    int e = errno;
    close(fd);
    return Err(errno_from(e));
  }
  socklen_t alen = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
  uint16_t bound = ntohs(addr.sin6_port);

  return std::unique_ptr<Listener>(new Listener(fd, bound, cfg, disp, rt));
}

Listener::~Listener() {
  if (fd_ >= 0) close(fd_);
}

void Listener::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load()) {
    // Unblock the in-flight accept so run() can observe the token.
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
  }
}

rt::Task<void> Listener::run() {
  run_reactor_.store(&current_reactor());
  auto token = stop_.token();
  for (;;) {
    Peer peer;
    peer.len = sizeof(peer.addr);
    int cfd = co_await uring_accept(fd_, reinterpret_cast<sockaddr*>(&peer.addr), &peer.len);
    if (token.cancel_requested()) {
      if (cfd >= 0) close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) {
      LNFS_WARN("accept failed: {}", errno_name(errno_from_neg(cfd)));
      continue;
    }
    obs::Metrics::instance().conns_accepted.fetch_add(1, std::memory_order_relaxed);
    if (!tracker_.try_add(peer)) {
      obs::Metrics::instance().conns_rejected.fetch_add(1, std::memory_order_relaxed);
      LNFS_WARN("conn limit reached, rejecting {}", peer.to_string());
      close(cfd);
      continue;
    }
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Reactor& r = rt_.next();  // round-robin assignment (design 03 §3.1)
    auto ctx = std::make_unique<ConnCtx>(cfd, peer, pool_, cfg_);
    spawn(connection_main(std::move(ctx), disp_, &tracker_), r);
  }
}

}  // namespace lnfs::transport
