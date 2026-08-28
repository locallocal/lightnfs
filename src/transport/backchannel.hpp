#pragma once
// Backchannel send side (design 07 §7.7, plan doc 10 §5.2): NFSv4.1 callbacks travel
// as RPC CALLs on a connection the *client* established.  A CbChannel is the send
// handle for one connection: the v4 state layer keeps a shared_ptr (it outlives the
// connection), sends full RPC records through call(), and the connection's read loop
// routes RPC REPLY records back by xid.
//
// Lifetime discipline: every fd touch happens on the connection's home reactor — the
// actual send is posted there and tracked by the connection's drain counter, so
// teardown waits for it and a queued-but-unstarted send observes detach() (same
// thread, total order) and skips.  After detach() every pending and future call fails
// with ECONNRESET.  call() itself never arms timers: the caller enforces liveness
// (the v4 state layer treats a stuck slot as a dead backchannel).

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "runtime/sync.hpp"
#include "runtime/task.hpp"
#include "util/result.hpp"

namespace lnfs::rt {
class Reactor;
}

namespace lnfs::transport {

struct ConnCtx;

class CbChannel {
 public:
  static std::shared_ptr<CbChannel> create(ConnCtx* conn, rt::Reactor* home) {
    auto chan = std::make_shared<CbChannel>(conn, home);
    chan->self_ = chan;
    return chan;
  }
  CbChannel(ConnCtx* conn, rt::Reactor* home) : conn_(conn), home_(home) {}

  // Monotonic xid for the next call; the caller encodes it into `record`.
  uint32_t next_xid() {
    std::lock_guard lock(mu_);
    return next_xid_++;
  }

  // Sends one full RPC CALL record (no record marking) and awaits the raw reply
  // record.  Callable from any reactor.
  rt::Task<Result<std::vector<std::byte>>> call(uint32_t xid,
                                                std::vector<std::byte> record);

  // Read-loop upcall (connection's reactor): reply record for `xid` arrived.
  // Returns false when no such call is pending (stale/unknown xid: dropped).
  bool route_reply(uint32_t xid, std::vector<std::byte> record);

  // Connection teardown (connection's reactor): fail pending and future calls.
  void detach();

  bool alive() {
    std::lock_guard lock(mu_);
    return conn_ != nullptr;
  }

  // Internal (connection-reactor only): the connection to send on, or null.
  ConnCtx* conn_for_send();

 private:
  struct Pending {
    std::vector<std::byte> reply;
    bool failed = false;
    rt::Event done;
  };

  std::mutex mu_;
  ConnCtx* conn_;      // null after detach
  rt::Reactor* home_;  // the connection's reactor: all fd work happens there
  uint32_t next_xid_ = 0x6c6e0001;  // "ln.."; distinct from client xids by role anyway
  std::unordered_map<uint32_t, Pending*> pending_;
  std::weak_ptr<CbChannel> self_;  // pins the channel across the posted send task
};

}  // namespace lnfs::transport
