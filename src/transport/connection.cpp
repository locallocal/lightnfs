#include "transport/connection.hpp"

#include "obs/metrics.hpp"

#include <arpa/inet.h>
#include <unistd.h>

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

std::string Peer::ip_key() const {
  char buf[INET6_ADDRSTRLEN] = "?";
  if (addr.ss_family == AF_INET) {
    auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
  } else if (addr.ss_family == AF_INET6) {
    auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
    inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

bool ConnTracker::try_add(const Peer& p) {
  std::lock_guard lk(mu_);
  if (total_ >= cfg_.max_connections) return false;
  int& c = per_peer_[p.ip_key()];
  if (c >= cfg_.per_peer_limit) return false;
  ++c;
  ++total_;
  obs::Metrics::instance().conns_active.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ConnTracker::remove(const Peer& p) {
  std::lock_guard lk(mu_);
  auto it = per_peer_.find(p.ip_key());
  if (it != per_peer_.end() && --it->second == 0) per_peer_.erase(it);
  --total_;
  obs::Metrics::instance().conns_active.fetch_sub(1, std::memory_order_relaxed);
}

int ConnTracker::count() {
  std::lock_guard lk(mu_);
  return total_;
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

namespace {
Task<void> handle_one(ConnCtx* c, rpc::Dispatcher* d, BufferChain rec) {
  co_await d->handle_request(*c, std::move(rec));
  c->inflight.release();
  if (--c->live == 0) c->drained.set();
}
}  // namespace

Task<void> connection_main(std::unique_ptr<ConnCtx> ctx, rpc::Dispatcher& disp,
                           ConnTracker* tracker) {
  ConnCtx* c = ctx.get();
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
      if (c->inflight.available() <= 0)
      obs::Metrics::instance().backpressure_waits.fetch_add(1, std::memory_order_relaxed);
    co_await c->inflight.acquire();  // backpressure (design 01 §1.5)
    ++c->live;
    spawn(handle_one(c, &disp, std::move(*rec)), current_reactor());
  }

  // Teardown (design 02 §2.6): cancel, then drain in-flight handlers.
  c->cancel.request();
  co_await uring_cancel_fd(c->fd);
  while (c->live > 0) {
    c->drained.reset();
    if (c->live > 0) co_await c->drained.wait();
  }
  co_await uring_close(c->fd);
  if (tracker) tracker->remove(c->peer);
}

}  // namespace lnfs::transport
