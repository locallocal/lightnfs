#include "server/ctl.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <format>

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
            "export={} hits={} misses={} upgrades={} evictions={} entries={}\n",
            entry->path, s.hits, s.misses, s.upgrades, s.evictions, s.entries);
      }
    }
    return out.empty() ? "no local exports\n" : out;
  }
  return "unknown command (ping|metrics|dump-errors|drc|fdcache|state|expire-client <clientid>)\n";
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
  std::array<std::byte, 256> buf{};
  int n = co_await uring_recv(cfd, buf);
  if (n > 0) {
    std::string_view line(reinterpret_cast<const char*>(buf.data()),
                          static_cast<size_t>(n));
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.remove_suffix(1);
    co_await send_all(cfd, co_await answer_async(deps_, std::string(line)));
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

Result<std::unique_ptr<MetricsHttp>> MetricsHttp::create(uint16_t port) {
  int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int zero = 0;
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, 64) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  socklen_t alen = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
  return std::unique_ptr<MetricsHttp>(new MetricsHttp(fd, ntohs(addr.sin6_port)));
}

MetricsHttp::~MetricsHttp() {
  if (fd_ >= 0) ::close(fd_);
}

void MetricsHttp::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load())
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
}

rt::Task<void> MetricsHttp::serve(int cfd) {
  std::array<std::byte, 1024> buf{};
  (void)co_await uring_recv(cfd, buf);  // request line + headers, contents ignored
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

}  // namespace lnfs::server
