#include "server/ctl.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <sstream>
#include <vector>

#include "backend/cephfs.hpp"
#include "backend/gluster.hpp"
#include "backend/local.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "state/state_mgr.hpp"
#include "transport/connection.hpp"
#include "runtime/io.hpp"
#include "util/log.hpp"

#ifndef LIGHTNFS_VERSION
#define LIGHTNFS_VERSION "dev"
#endif

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

// Command line -> tokens; a `--json` token anywhere selects the JSON rendering
// (plan doc 10 §4.2 — scripts stop grepping free text).
struct Cmd {
  std::vector<std::string> args;
  bool json = false;
  const std::string& name() const {
    static const std::string empty;
    return args.empty() ? empty : args[0];
  }
  std::string_view arg(size_t i) const {
    return i < args.size() ? std::string_view(args[i]) : std::string_view();
  }
};

Cmd parse_command(std::string_view line) {
  Cmd out;
  std::istringstream in{std::string(line)};
  std::string tok;
  while (in >> tok) {
    if (tok == "--json") out.json = true;
    else out.args.push_back(std::move(tok));
  }
  return out;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
          out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
        else out += c;
    }
  }
  return out;
}

const char* kHelp =
    "unknown command; available: ping|version|status|metrics|dump-errors|drc [flush]|"
    "fdcache [flush]|clear-poison|state|expire-client <clientid>|conns|kill-conn <id>|"
    "loglevel <debug|info|warn|error>|reload|drain|grace-end  (append --json for JSON "
    "output)\n";

}  // namespace

std::string CtlServer::answer(const CtlDeps& deps, std::string_view command) {
  const Cmd cmd = parse_command(command);
  const bool json = cmd.json;
  if (cmd.name() == "ping") return json ? "{\"ok\":true}\n" : "pong\n";
  if (cmd.name() == "version") {
    return json ? std::format("{{\"version\":\"{}\"}}\n", LIGHTNFS_VERSION)
                : std::format("lightnfs {}\n", LIGHTNFS_VERSION);
  }
  if (cmd.name() == "status") {
    auto uptime =
        deps.started.time_since_epoch().count() == 0
            ? 0
            : std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - deps.started)
                  .count();
    size_t conns = transport::ConnRegistry::instance().count();
    bool draining = deps.draining && deps.draining->load(std::memory_order_relaxed);
    size_t exports = deps.exports ? deps.exports->entries().size() : 0;
    bool grace = deps.state && deps.state->in_grace();
    int64_t grace_left = deps.state ? deps.state->grace_remaining_seconds() : 0;
    if (json)
      return std::format(
          "{{\"version\":\"{}\",\"uptime_s\":{},\"connections\":{},\"draining\":{},"
          "\"exports\":{},\"grace\":{},\"grace_remaining_s\":{}}}\n",
          LIGHTNFS_VERSION, uptime, conns, draining, exports, grace, grace_left);
    return std::format(
        "version={} uptime={}s connections={} draining={} exports={} grace={} "
        "grace_remaining={}s\n",
        LIGHTNFS_VERSION, uptime, conns, draining ? 1 : 0, exports, grace ? 1 : 0,
        grace_left);
  }
  if (cmd.name() == "metrics") return obs::prometheus_text();
  if (cmd.name() == "dump-errors")
    return json ? obs::dump_error_replies_json() : obs::dump_error_replies();
  if (cmd.name() == "loglevel") {
    auto lv = cmd.arg(1);
    if (lv == "debug") set_log_level(LogLevel::kDebug);
    else if (lv == "info") set_log_level(LogLevel::kInfo);
    else if (lv == "warn") set_log_level(LogLevel::kWarn);
    else if (lv == "error") set_log_level(LogLevel::kError);
    else return json ? "{\"error\":\"bad level\"}\n"
                     : "loglevel: expected debug|info|warn|error\n";
    LNFS_INFO("log level set to {} via ctl", lv);
    return json ? std::format("{{\"loglevel\":\"{}\"}}\n", lv)
                : std::format("log level set to {}\n", lv);
  }
  if (cmd.name() == "reload") {
    if (!deps.reload) return json ? "{\"error\":\"unavailable\"}\n" : "reload unavailable\n";
    std::string report = deps.reload();
    return json ? std::format("{{\"reloaded\":true,\"report\":\"{}\"}}\n",
                              json_escape(report))
                : report;
  }
  if (cmd.name() == "drain") {
    if (!deps.drain) return json ? "{\"error\":\"unavailable\"}\n" : "drain unavailable\n";
    std::string report = deps.drain();
    return json ? std::format("{{\"draining\":true,\"report\":\"{}\"}}\n",
                              json_escape(report))
                : report;
  }
  if (cmd.name() == "grace-end") {
    if (!deps.state) return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    bool was = deps.state->end_grace();
    if (json) return std::format("{{\"grace_ended\":{}}}\n", was);
    return was ? "grace period ended\n" : "not in grace\n";
  }
  if (cmd.name() == "conns") {
    auto conns = transport::ConnRegistry::instance().list();
    std::string out;
    if (json) {
      out = "[";
      for (size_t i = 0; i < conns.size(); ++i)
        out += std::format("{}{{\"id\":{},\"peer\":\"{}\",\"age_s\":{}}}",
                           i ? "," : "", conns[i].id, json_escape(conns[i].peer),
                           conns[i].age_s);
      out += "]\n";
      return out;
    }
    for (const auto& c : conns)
      out += std::format("id={} peer={} age={}s\n", c.id, c.peer, c.age_s);
    return out.empty() ? "no connections\n" : out;
  }
  if (cmd.name() == "kill-conn") {
    uint64_t id = 0;
    auto arg = cmd.arg(1);
    auto [p, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), id);
    if (ec != std::errc() || p != arg.data() + arg.size())
      return json ? "{\"error\":\"bad id\"}\n" : "kill-conn: expected a numeric id (see conns)\n";
    bool killed = transport::ConnRegistry::instance().kill(id);
    if (json) return std::format("{{\"killed\":{}}}\n", killed);
    return killed ? std::format("connection {} shut down\n", id)
                  : std::format("connection {} not found\n", id);
  }
  if (cmd.name() == "drc") {
    if (!deps.drc) return json ? "{\"error\":\"drc disabled\"}\n" : "drc disabled\n";
    auto s = deps.drc->stats();
    if (json)
      return std::format(
          "{{\"inserts\":{},\"replays\":{},\"waits\":{},\"evictions\":{},"
          "\"entries\":{},\"bytes\":{}}}\n",
          s.inserts, s.replays, s.waits, s.evictions, s.entries, s.bytes);
    return std::format(
        "inserts={} replays={} waits={} evictions={} entries={} bytes={}\n", s.inserts,
        s.replays, s.waits, s.evictions, s.entries, s.bytes);
  }
  if (cmd.name() == "fdcache" && cmd.arg(1) == "flush") {
    size_t total = 0;
    bool any = false;
    if (deps.exports) {
      for (const auto& entry : deps.exports->entries()) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          any = true;
          total += g->flush_fd_cache();
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          any = true;
          total += c->flush_fd_cache();
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        any = true;
        total += local->flush_fd_cache();
      }
    }
    if (json) return std::format("{{\"flushed\":{}}}\n", total);
    return any ? std::format("flushed {} cached fds\n", total) : "no local/gluster/cephfs exports\n";
  }
  if (cmd.name() == "fdcache") {
    std::string out;
    if (json) out = "[";
    size_t emitted = 0;
    if (deps.exports) {
      for (const auto& entry : deps.exports->entries()) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          auto s = g->stats();
          if (json) {
            out += std::format(
                "{}{{\"export\":\"{}\",\"backend\":\"gluster\",\"hits\":{},\"misses\":{},"
                "\"upgrades\":{},\"evictions\":{},\"entries\":{},\"obj_hits\":{},"
                "\"obj_misses\":{},\"obj_entries\":{},\"jukebox\":{},\"lock_fds\":{}}}",
                emitted ? "," : "", json_escape(entry->path), s.fd_hits, s.fd_misses,
                s.fd_upgrades, s.fd_evictions, s.fd_entries, s.obj_hits, s.obj_misses,
                s.obj_entries, s.jukebox, s.lock_fds);
          } else {
            out += std::format(
                "export={} backend=gluster hits={} misses={} upgrades={} evictions={} "
                "entries={} obj_hits={} obj_misses={} obj_entries={} jukebox={} lock_fds={}\n",
                entry->path, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions,
                s.fd_entries, s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox, s.lock_fds);
          }
          ++emitted;
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          auto s = c->stats();
          if (json) {
            out += std::format(
                "{}{{\"export\":\"{}\",\"backend\":\"cephfs\",\"hits\":{},\"misses\":{},"
                "\"upgrades\":{},\"evictions\":{},\"entries\":{},\"obj_hits\":{},"
                "\"obj_misses\":{},\"obj_entries\":{},\"jukebox\":{},\"blocklisted\":{},"
                "\"lock_fds\":{}}}",
                emitted ? "," : "", json_escape(entry->path), s.fd_hits, s.fd_misses,
                s.fd_upgrades, s.fd_evictions, s.fd_entries, s.obj_hits, s.obj_misses,
                s.obj_entries, s.jukebox, s.blocklisted, s.lock_fds);
          } else {
            out += std::format(
                "export={} backend=cephfs hits={} misses={} upgrades={} evictions={} "
                "entries={} obj_hits={} obj_misses={} obj_entries={} jukebox={} "
                "blocklisted={} lock_fds={}\n",
                entry->path, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions,
                s.fd_entries, s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox,
                s.blocklisted, s.lock_fds);
          }
          ++emitted;
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        auto s = local->fd_cache_stats();
        if (json) {
          out += std::format(
              "{}{{\"export\":\"{}\",\"hits\":{},\"misses\":{},\"upgrades\":{},"
              "\"evictions\":{},\"overflows\":{},\"entries\":{},\"path_hits\":{},"
              "\"path_misses\":{},\"path_entries\":{}}}",
              emitted ? "," : "", json_escape(entry->path), s.hits, s.misses,
              s.upgrades, s.evictions, s.overflows, s.entries, s.path_hits,
              s.path_misses, s.path_entries);
        } else {
          out += std::format(
              "export={} hits={} misses={} upgrades={} evictions={} overflows={} "
              "entries={} path_hits={} path_misses={} path_entries={}\n",
              entry->path, s.hits, s.misses, s.upgrades, s.evictions, s.overflows,
              s.entries, s.path_hits, s.path_misses, s.path_entries);
        }
        ++emitted;
      }
    }
    if (json) return out + "]\n";
    return out.empty() ? "no local/gluster/cephfs exports\n" : out;
  }
  if (cmd.name() == "clear-poison") {
    // Sticky fsync-EIO marks (design 06 §6.2) previously survived until restart
    // (plan doc 10 §1.5); this is the operator's way out after fixing the media.
    size_t total = 0;
    bool any = false;
    if (deps.exports) {
      for (const auto& entry : deps.exports->entries()) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          any = true;
          total += g->clear_poison();
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          any = true;
          total += c->clear_poison();
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        any = true;
        total += local->clear_poison();
      }
    }
    if (json) return std::format("{{\"cleared\":{}}}\n", total);
    return any ? std::format("cleared {} poison marks\n", total) : "no local/gluster/cephfs exports\n";
  }
  return json ? "{\"error\":\"unknown command\"}\n" : kHelp;
}

rt::Task<std::string> CtlServer::answer_async(const CtlDeps& deps, std::string command) {
  const Cmd cmd = parse_command(command);
  const bool json = cmd.json;
  if (cmd.name() == "state") {
    if (!deps.state) co_return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    auto s = deps.state->stats();
    if (json) {
      // Counters only: the table dumps stay a human-format text view.
      co_return std::format(
          "{{\"clients\":{},\"sessions\":{},\"opens\":{},\"files\":{},\"courtesy\":{},"
          "\"grace\":{},\"grace_remaining_s\":{},\"lease_expirations\":{},"
          "\"reclaim_conflict\":{},\"reclaim_timeout\":{},\"reclaim_forced\":{},"
          "\"share_denied\":{},\"open_merges\":{},\"lock_states\":{},"
          "\"lock_segments\":{},\"lock_owners\":{},\"lock_denied\":{}}}\n",
          s.clients, s.sessions, s.opens, s.files, s.courtesy, s.grace,
          s.grace_remaining, s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout,
          s.reclaim_forced, s.share_denied, s.open_merges, s.lock_states,
          s.lock_segments, s.lock_owners, s.lock_denied);
    }
    std::string out = std::format(
        "clients={} sessions={} opens={} files={} courtesy={} grace={} grace_remaining={}s "
        "lease_expirations={} reclaim_conflict={} reclaim_timeout={} reclaim_forced={} "
        "share_denied={} open_merges={} lock_states={} lock_segments={} lock_owners={} "
        "lock_denied={} delegs={} deleg_grants={} deleg_recalls={} deleg_returns={} "
        "deleg_revokes={} cb_lock_notifies={}\n",
        s.clients, s.sessions, s.opens, s.files, s.courtesy, s.grace ? 1 : 0,
        s.grace_remaining, s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout,
        s.reclaim_forced, s.share_denied, s.open_merges, s.lock_states, s.lock_segments,
        s.lock_owners, s.lock_denied, s.delegs, s.deleg_grants, s.deleg_recalls,
        s.deleg_returns, s.deleg_revokes, s.cb_lock_notifies);
    out += co_await deps.state->dump();
    co_return out;
  }
  if (cmd.name() == "expire-client") {
    if (!deps.state) co_return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    uint64_t id = 0;
    try {
      id = std::stoull(std::string(cmd.arg(1)), nullptr, 0);
    } catch (...) {
      co_return json ? "{\"error\":\"bad clientid\"}\n" : "expire-client: bad clientid\n";
    }
    uint32_t status = co_await deps.state->expire_client(id);
    if (json) co_return std::format("{{\"clientid\":\"{:#x}\",\"status\":{}}}\n", id, status);
    co_return status == 0 ? std::format("client {:#x} reclaimed\n", id)
                          : std::format("client {:#x}: nfs4 status {}\n", id, status);
  }
  if (cmd.name() == "drc" && cmd.arg(1) == "flush") {
    if (!deps.drc) co_return json ? "{\"error\":\"drc disabled\"}\n" : "drc disabled\n";
    size_t dropped = co_await deps.drc->flush();
    if (json) co_return std::format("{{\"flushed\":{}}}\n", dropped);
    co_return std::format("flushed {} drc entries\n", dropped);
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
