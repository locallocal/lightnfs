#include "transport/connection.hpp"

#include "obs/metrics.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "runtime/io.hpp"
#include "util/log.hpp"

namespace lnfs::transport {

using namespace lnfs::rt;

std::string Peer::to_string() const {
  char buf[INET6_ADDRSTRLEN] = "?";
  uint16_t port = 0;
  if (addr.ss_family == AF_INET) {
    auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    port = ntohs(a->sin_port);
  } else if (addr.ss_family == AF_INET6) {
    auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
    inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    port = ntohs(a->sin6_port);
  }
  return std::string(buf) + ":" + std::to_string(port);
}

IpKey Peer::ip_key() const {
  IpKey k;
  if (addr.ss_family == AF_INET) {
    auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    k.b[10] = k.b[11] = 0xff;  // ::ffff:a.b.c.d
    std::memcpy(k.b.data() + 12, &a->sin_addr, 4);
  } else if (addr.ss_family == AF_INET6) {
    auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
    std::memcpy(k.b.data(), &a->sin6_addr, 16);
  }
  return k;
}

size_t IpKeyHash::operator()(const IpKey& k) const noexcept {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : k.b) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return static_cast<size_t>(h);
}

bool ConnTracker::try_add(const Peer& p) {
  if (total_.fetch_add(1, std::memory_order_relaxed) >= cfg_.max_connections) {
    total_.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  IpKey key = p.ip_key();
  Shard& sh = shard_of(key);
  {
    std::lock_guard lk(sh.mu);
    int& c = sh.per_peer[key];
    if (c >= cfg_.per_peer_limit) {
      if (c == 0) sh.per_peer.erase(key);
      total_.fetch_sub(1, std::memory_order_relaxed);
      return false;
    }
    ++c;
  }
  obs::Metrics::instance().conns_active.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ConnTracker::remove(const Peer& p) {
  IpKey key = p.ip_key();
  Shard& sh = shard_of(key);
  {
    std::lock_guard lk(sh.mu);
    auto it = sh.per_peer.find(key);
    if (it != sh.per_peer.end() && --it->second == 0) sh.per_peer.erase(it);
  }
  total_.fetch_sub(1, std::memory_order_relaxed);
  obs::Metrics::instance().conns_active.fetch_sub(1, std::memory_order_relaxed);
}

int ConnTracker::count() { return total_.load(std::memory_order_relaxed); }

ConnRegistry& ConnRegistry::instance() {
  static ConnRegistry r;
  return r;
}

uint64_t ConnRegistry::add(int fd, const Peer& peer) {
  std::lock_guard lock(mu_);
  uint64_t id = next_id_++;
  conns_.emplace(id, Ent{fd, peer, std::chrono::steady_clock::now()});
  return id;
}

void ConnRegistry::remove(uint64_t id) {
  std::lock_guard lock(mu_);
  conns_.erase(id);
}

std::vector<ConnRegistry::Info> ConnRegistry::list() {
  std::lock_guard lock(mu_);
  auto now = std::chrono::steady_clock::now();
  std::vector<Info> out;
  out.reserve(conns_.size());
  for (const auto& [id, ent] : conns_)
    out.push_back({id, ent.peer.to_string(),
                   std::chrono::duration_cast<std::chrono::seconds>(now - ent.since)
                       .count()});
  std::sort(out.begin(), out.end(),
            [](const Info& a, const Info& b) { return a.id < b.id; });
  return out;
}

size_t ConnRegistry::count() {
  std::lock_guard lock(mu_);
  return conns_.size();
}

bool ConnRegistry::kill(uint64_t id) {
  std::lock_guard lock(mu_);
  auto it = conns_.find(id);
  if (it == conns_.end()) return false;
  ::shutdown(it->second.fd, SHUT_RDWR);
  return true;
}

Task<void> ConnCtx::send(rt::SendBuf buf) {
  auto r = co_await rs.write_record(std::move(buf));
  if (!r && !send_failed) {
    send_failed = true;
    LNFS_WARN("conn {}: send failed ({}), tearing down", peer.to_string(),
              errno_name(r.error()));
    cancel.request();
  }
}

std::shared_ptr<CbChannel> ConnCtx::cb_channel() {
  if (!cb) cb = CbChannel::create(this, rt::current_reactor_or_null());
  return cb;
}

void ConnCtx::route_cb_reply(rt::BufferChain rec) {
  auto bytes = rec.to_bytes();
  if (bytes.size() < 8 || !cb) return;  // no channel: nothing ever called out
  uint32_t xid = 0;
  std::memcpy(&xid, bytes.data(), 4);
  xid = xdr::to_be32(xid);  // symmetric byte swap
  if (!cb->route_reply(xid, std::move(bytes)))
    LNFS_DEBUG("conn {}: unmatched callback reply xid={:#x}, dropped", peer.to_string(),
               xid);
}

namespace {
Task<void> handle_one(ConnCtx* c, rpc::Dispatcher* d, BufferChain rec) {
  // Failure isolation (plan doc 10 §2.4): an exception that escapes the dispatcher's
  // own error handling (e.g. bad_alloc while encoding the error reply) costs this
  // connection, not the process.
  try {
    co_await d->handle_request(*c, std::move(rec));
  } catch (const std::exception& e) {
    LNFS_ERROR("conn {}: unrecoverable handler error ({}), tearing down",
               c->peer.to_string(), e.what());
    c->cancel.request();
  } catch (...) {
    LNFS_ERROR("conn {}: unrecoverable handler error, tearing down", c->peer.to_string());
    c->cancel.request();
  }
  c->inflight.release();
  if (--c->live == 0) c->drained.set();
}
}  // namespace

Task<void> connection_main(std::unique_ptr<ConnCtx> ctx, rpc::Dispatcher& disp,
                           ConnTracker* tracker) {
  ConnCtx* c = ctx.get();
  uint64_t conn_id = ConnRegistry::instance().add(c->fd, c->peer);
  for (;;) {
    auto rec = co_await c->rs.read_record();
    if (!rec) {
      if (rec.error() != Errno::kEof) {
        LNFS_WARN("conn {}: framing error ({}), closing", c->peer.to_string(),
                  errno_name(rec.error()));
      }
      break;
    }
    if (rec->empty()) continue;  // empty record: ignore
    if (c->cancel.cancel_requested()) break;
    {
      // RPC REPLY records answer our backchannel calls (plan doc 10 §5.2): routed by
      // xid to the pending callback, never dispatched as requests.
      xdr::XdrDec peek(*rec);
      (void)peek.u32();  // xid
      auto mtype = peek.u32();
      if (mtype && *mtype == rpc::kReply) {
        c->route_cb_reply(std::move(*rec));
        continue;
      }
    }
      if (c->inflight.available() <= 0)
      obs::Metrics::instance().backpressure_waits.fetch_add(1, std::memory_order_relaxed);
    co_await c->inflight.acquire();  // backpressure (design 01 §1.5)
    ++c->live;
    spawn(handle_one(c, &disp, std::move(*rec)), current_reactor());
  }

  // Teardown (design 02 §2.6): cancel, then drain in-flight handlers.  The backchannel
  // detaches first (same reactor): pending callback waiters fail, a queued-but-unrun
  // callback send observes the detach and skips, and an in-flight one is counted in
  // `live` so the drain below waits for it.
  if (c->cb) c->cb->detach();
  c->cancel.request();
  co_await uring_cancel_fd(c->fd);
  while (c->live > 0) {
    c->drained.reset();
    if (c->live > 0) co_await c->drained.wait();
  }
  ConnRegistry::instance().remove(conn_id);  // before close: kill() must not hit a reused fd
  co_await uring_close(c->fd);
  if (tracker) tracker->remove(c->peer);
}

}  // namespace lnfs::transport
