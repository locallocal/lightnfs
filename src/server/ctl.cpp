#include "server/ctl.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <algorithm>
#include <format>
#include <memory>

#include "backend/local.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "state/state_mgr.hpp"
#include "runtime/io.hpp"
#include "util/log.hpp"

namespace lnfs::server {

using namespace lnfs::rt;

namespace {

Task<void> send_all(int fd, std::string_view text) {
  size_t off = 0;
  while (off < text.size()) {
    iovec iov{const_cast<char*>(text.data()) + off, text.size() - off};
    int n = co_await uring_sendv(fd, &iov, 1);
    if (n <= 0) co_return;
    off += static_cast<size_t>(n);
  }
}

}  // namespace

std::string CtlServer::answer(const CtlDeps& deps, std::string_view command) {
  if (command == "ping") return "pong\n";
  if (command == "metrics") return obs::prometheus_text();
  if (command == "dump-errors") return obs::dump_error_replies();
  if (command == "drc") {
    if (!deps.drc) return "drc disabled\n";
    auto s = deps.drc->stats();
    return std::format(
        "inserts={} replays={} waits={} evictions={} entries={} bytes={}\n", s.inserts,
        s.replays, s.waits, s.evictions, s.entries, s.bytes);
  }
  if (command == "fdcache") {
    std::string out;
    if (deps.exports) {
      for (const auto& entry : deps.exports->entries()) {
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        auto s = local->fd_cache_stats();
        out += std::format(
            "export={} hits={} misses={} upgrades={} evictions={} overflows={} "
            "entries={} path_hits={} path_misses={} path_entries={}\n",
            entry->path, s.hits, s.misses, s.upgrades, s.evictions, s.overflows,
            s.entries, s.path_hits, s.path_misses, s.path_entries);
      }
    }
    return out.empty() ? "no local exports\n" : out;
  }
  if (command == "clear-poison") {
    // Sticky fsync-EIO marks (design 06 §6.2) previously survived until restart
    // (plan doc 10 §1.5); this is the operator's way out after fixing the media.
    size_t total = 0;
    bool any = false;
    if (deps.exports) {
      for (const auto& entry : deps.exports->entries()) {
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        any = true;
        total += local->clear_poison();
      }
    }
    return any ? std::format("cleared {} poison marks\n", total) : "no local exports\n";
  }
  return "unknown command (ping|metrics|dump-errors|drc|fdcache|clear-poison|state|"
         "expire-client <clientid>)\n";
}

rt::Task<std::string> CtlServer::answer_async(const CtlDeps& deps, std::string command) {
  if (command == "state") {
    if (!deps.state) co_return "v4 disabled\n";
    auto s = deps.state->stats();
    std::string out = std::format(
        "clients={} sessions={} opens={} files={} courtesy={} grace={} grace_remaining={}s "
        "lease_expirations={} reclaim_conflict={} reclaim_timeout={} reclaim_forced={} "
        "share_denied={} open_merges={}\n",
        s.clients, s.sessions, s.opens, s.files, s.courtesy, s.grace ? 1 : 0,
        s.grace_remaining, s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout,
        s.reclaim_forced, s.share_denied, s.open_merges);
    out += co_await deps.state->dump();
    co_return out;
  }
  if (command.starts_with("expire-client ")) {
    if (!deps.state) co_return "v4 disabled\n";
    std::string arg = command.substr(14);
    uint64_t id = 0;
    try {
      id = std::stoull(arg, nullptr, 0);
    } catch (...) {
      co_return "expire-client: bad clientid\n";
    }
    uint32_t status = co_await deps.state->expire_client(id);
    co_return status == 0 ? std::format("client {:#x} reclaimed\n", id)
                          : std::format("client {:#x}: nfs4 status {}\n", id, status);
  }
  co_return answer(deps, command);
}

Result<std::unique_ptr<CtlServer>> CtlServer::create(const std::string& socket_path,
                                                     CtlDeps deps) {
  sockaddr_un addr{};
  if (socket_path.size() >= sizeof(addr.sun_path)) return Err(errno_from(ENAMETOOLONG));
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  ::unlink(socket_path.c_str());  // stale socket from a previous run
  addr.sun_family = AF_UNIX;
  socket_path.copy(addr.sun_path, socket_path.size());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, 16) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  // Owner-only socket permissions independent of umask (plan doc 10 §1.8); the tiny
  // bind→chmod window is covered by the SO_PEERCRED gate in serve().
  (void)::chmod(socket_path.c_str(), 0600);
  return std::unique_ptr<CtlServer>(new CtlServer(fd, socket_path, deps));
}

CtlServer::~CtlServer() {
  if (fd_ >= 0) ::close(fd_);
  if (!path_.empty()) ::unlink(path_.c_str());
}

void CtlServer::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load())
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
}

rt::Task<void> CtlServer::serve(int cfd) {
  // Only root or the server's own user may issue ctl commands — expire-client is
  // destructive, and the socket path permissions alone depend on the filesystem
  // (plan doc 10 §1.8).
  ucred peer{};
  socklen_t plen = sizeof peer;
  if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &peer, &plen) != 0 ||
      (peer.uid != 0 && peer.uid != ::geteuid())) {
    co_await send_all(cfd, "permission denied\n");
    co_await uring_close(cfd);
    co_return;
  }
  // One command per connection, terminated by newline or EOF (lightnfs-ctl shuts down
  // its write side): loop so a fragmented send is not silently truncated.
  std::string line;
  std::array<std::byte, 256> buf{};
  size_t received = 0;
  while (line.size() < 4096 && line.find('\n') == std::string::npos) {
    int n = co_await uring_recv(cfd, buf);
    if (n <= 0) break;
    received += static_cast<size_t>(n);
    line.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
  }
  if (received > 0) {
    if (auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    co_await send_all(cfd, co_await answer_async(deps_, std::move(line)));
  }
  co_await uring_close(cfd);
}

rt::Task<void> CtlServer::run() {
  run_reactor_.store(&current_reactor());
  auto token = stop_.token();
  for (;;) {
    int cfd = co_await uring_accept(fd_, nullptr, nullptr);
    if (token.cancel_requested()) {
      if (cfd >= 0) ::close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) continue;
    spawn(serve(cfd), current_reactor());
  }
}

Result<std::unique_ptr<MetricsHttp>> MetricsHttp::create(uint16_t port,
                                                         const std::string& bind_addr,
                                                         std::vector<core::Cidr> allow) {
  // Bind the configured address instead of in6addr_any (plan doc 10 §1.8): the config
  // default is loopback, so exposing metrics beyond the host is an explicit choice.
  sockaddr_storage ss{};
  socklen_t slen = 0;
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
  auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
  if (inet_pton(AF_INET6, bind_addr.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    slen = sizeof(*v6);
  } else if (inet_pton(AF_INET, bind_addr.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    slen = sizeof(*v4);
  } else {
    return Err(errno_from(EINVAL));
  }
  int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (ss.ss_family == AF_INET6) {
    int zero = 0;  // "::" keeps serving mapped v4 peers as before
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), slen) < 0 || ::listen(fd, 64) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  socklen_t alen = slen;
  getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &alen);
  uint16_t bound = ntohs(ss.ss_family == AF_INET6 ? v6->sin6_port : v4->sin_port);
  return std::unique_ptr<MetricsHttp>(new MetricsHttp(fd, bound, std::move(allow)));
}

MetricsHttp::~MetricsHttp() {
  if (fd_ >= 0) ::close(fd_);
}

void MetricsHttp::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load())
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
}

bool MetricsHttp::allowed(const sockaddr_storage& peer) const {
  if (allow_.empty()) return true;
  return std::any_of(allow_.begin(), allow_.end(),
                     [&](const core::Cidr& c) { return c.contains(peer); });
}

rt::Task<void> MetricsHttp::serve(int cfd) {
  // Request line + headers, contents ignored.  A half-open peer used to park this
  // coroutine forever (plan doc 10 §1.8): bound the read.  The buffer is shared with
  // the detached recv so a timed-out read can never scribble on a dead frame.
  auto buf = std::make_shared<std::array<std::byte, 1024>>();
  auto got = co_await rt::with_timeout(
      [](int fd, std::shared_ptr<std::array<std::byte, 1024>> b) -> rt::Task<int> {
        co_return co_await uring_recv(fd, std::span<std::byte>(b->data(), b->size()));
      }(cfd, buf),
      std::chrono::seconds(5));
  if (!got) {
    co_await uring_cancel_fd(cfd);  // release the parked recv before the fd goes away
    co_await uring_close(cfd);
    co_return;
  }
  std::string body = obs::prometheus_text();
  std::string response = std::format(
      "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
      "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
      body.size(), body);
  co_await send_all(cfd, response);
  co_await uring_close(cfd);
}

rt::Task<void> MetricsHttp::run() {
  run_reactor_.store(&current_reactor());
  auto token = stop_.token();
  for (;;) {
    sockaddr_storage peer{};
    socklen_t plen = sizeof peer;
    int cfd = co_await uring_accept(fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (token.cancel_requested()) {
      if (cfd >= 0) ::close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) continue;
    if (!allowed(peer)) {  // CIDR allowlist (plan doc 10 §1.8)
      ::close(cfd);
      continue;
    }
    spawn(serve(cfd), current_reactor());
  }
}

}  // namespace lnfs::server
