#include "transport/listener.hpp"

#include "obs/metrics.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "runtime/io.hpp"
#include "util/log.hpp"

namespace lnfs::transport {

using namespace lnfs::rt;

namespace {

// One listening socket bound to `bind_addr`:`port` with SO_REUSEPORT. An empty
// bind_addr keeps the historical dual-stack any-address socket; a literal narrows the
// listener to that interface (plan doc 10 §4.4).  `bound` receives the actual port
// (meaningful for the first socket when port == 0).
Result<int> make_listen_socket(const std::string& bind_addr, uint16_t port,
                               uint16_t& bound) {
  sockaddr_storage ss{};
  socklen_t slen = 0;
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
  auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
  bool dual_stack = false;
  if (bind_addr.empty() || bind_addr == "::") {
    v6->sin6_family = AF_INET6;
    v6->sin6_addr = in6addr_any;
    v6->sin6_port = htons(port);
    slen = sizeof(*v6);
    dual_stack = true;
  } else if (inet_pton(AF_INET6, bind_addr.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    slen = sizeof(*v6);
  } else if (inet_pton(AF_INET, bind_addr.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    slen = sizeof(*v4);
  } else {
    return Err(errno_from(EINVAL));  // config validation catches this earlier
  }
  int fd = socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  if (dual_stack) {
    int zero = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&ss), slen) < 0 || listen(fd, 1024) < 0) {
    int e = errno;
    close(fd);
    return Err(errno_from(e));
  }
  socklen_t alen = sizeof(ss);
  getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &alen);
  bound = ntohs(ss.ss_family == AF_INET6 ? v6->sin6_port : v4->sin_port);
  return fd;
}

}  // namespace

Result<std::unique_ptr<Listener>> Listener::create(uint16_t port, TransportConfig cfg,
                                                   rpc::Dispatcher& disp, rt::Runtime& rt,
                                                   const std::string& bind_addr) {
  std::vector<int> fds;
  uint16_t bound = 0;
  for (size_t i = 0; i < rt.reactor_count(); ++i) {
    // First bind resolves an ephemeral port; the rest share it via SO_REUSEPORT.
    auto fd = make_listen_socket(bind_addr, i == 0 ? port : bound, bound);
    if (!fd) {
      for (int f : fds) close(f);
      return Err(fd.error());
    }
    fds.push_back(*fd);
  }
  return std::unique_ptr<Listener>(new Listener(std::move(fds), bound, cfg, disp, rt));
}

Listener::~Listener() {
  for (int fd : fds_) {
    if (fd >= 0) close(fd);
  }
}

void Listener::start() {
  for (size_t i = 0; i < fds_.size(); ++i) spawn(run_one(i), rt_.reactor(i));
}

void Listener::request_stop() {
  stop_.request();
  for (size_t i = 0; i < fds_.size(); ++i) {
    // Unblock the in-flight accept so run_one() can observe the token.
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fds_[i]),
          rt_.reactor(i));
  }
}

rt::Task<void> Listener::run_one(size_t idx) {
  const int lfd = fds_[idx];
  auto token = stop_.token();
  for (;;) {
    // Addr-less accept keeps the multishot fast path (plan doc 10 §2.3); the peer
    // address comes from getpeername() on the new socket.
    int cfd = co_await uring_accept(lfd, nullptr, nullptr);
    if (token.cancel_requested()) {
      if (cfd >= 0) close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) {
      LNFS_WARN("accept failed: {}", errno_name(errno_from_neg(cfd)));
      continue;
    }
    Peer peer;
    peer.len = sizeof(peer.addr);
    if (getpeername(cfd, reinterpret_cast<sockaddr*>(&peer.addr), &peer.len) < 0) {
      peer = Peer{};  // already disconnected: track under the zero address
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
    // Serve where accepted: SO_REUSEPORT already spread the load across reactors.
    auto ctx = std::make_unique<ConnCtx>(cfd, peer, pool_, cfg_);
    spawn(connection_main(std::move(ctx), disp_, &tracker_), current_reactor());
  }
}

}  // namespace lnfs::transport
