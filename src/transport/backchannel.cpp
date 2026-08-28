#include "transport/backchannel.hpp"

#include <cstring>

#include "runtime/reactor.hpp"
#include "transport/connection.hpp"
#include "util/log.hpp"

namespace lnfs::transport {

namespace {

// The tracked half of a callback send: runs on the connection's reactor, counted by
// the connection's drain counter so teardown waits for it (see connection_main).
rt::Task<void> send_on_conn(CbChannel* chan, std::shared_ptr<CbChannel> keep,
                            std::vector<std::byte> record) {
  (void)keep;  // pins the channel while this task lives
  ConnCtx* c = chan->conn_for_send();
  if (!c) co_return;  // detach won the race: connection is gone
  ++c->live;
  auto buf = c->pool.alloc(record.size());
  std::memcpy(buf.data(), record.data(), record.size());
  rt::SendBuf out;
  out.append(std::move(buf), 0, static_cast<uint32_t>(record.size()));
  co_await c->send(std::move(out));
  if (--c->live == 0) c->drained.set();
}

}  // namespace

rt::Task<Result<std::vector<std::byte>>> CbChannel::call(uint32_t xid,
                                                         std::vector<std::byte> record) {
  Pending pending;
  rt::Reactor* home = nullptr;
  {
    std::lock_guard lock(mu_);
    if (!conn_) co_return Err(errno_from(ECONNRESET));
    pending_[xid] = &pending;
    home = home_;
  }
  // The send task re-checks the channel on the connection's own thread; teardown and
  // that check are totally ordered there, so it can never touch a freed connection.
  rt::spawn(send_on_conn(this, self_.lock(), std::move(record)), *home);
  co_await pending.done.wait();
  {
    std::lock_guard lock(mu_);
    pending_.erase(xid);
  }
  if (pending.failed) co_return Err(errno_from(ECONNRESET));
  co_return std::move(pending.reply);
}

ConnCtx* CbChannel::conn_for_send() {
  std::lock_guard lock(mu_);
  return conn_;  // non-null implies the connection has not begun teardown (same thread)
}

bool CbChannel::route_reply(uint32_t xid, std::vector<std::byte> record) {
  std::lock_guard lock(mu_);
  auto it = pending_.find(xid);
  if (it == pending_.end()) return false;
  it->second->reply = std::move(record);
  it->second->done.set();
  return true;
}

void CbChannel::detach() {
  std::lock_guard lock(mu_);
  conn_ = nullptr;
  for (auto& [xid, p] : pending_) {
    p->failed = true;
    p->done.set();
  }
  pending_.clear();
}

}  // namespace lnfs::transport
