#pragma once
// Per-connection context and main loop (design 03 §3.3, 01 §1.5).
//  - one connection_main coroutine per accepted socket, pinned to one reactor
//  - request pipelining: each parsed record spawns an independent handler coroutine
//  - backpressure: per-conn inflight Semaphore gates further record parsing (TCP pushback)
//  - teardown: cancel token -> best-effort ring cancel -> drain in-flight handlers

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rpc/dispatch.hpp"
#include "runtime/buffer.hpp"
#include "runtime/cancel.hpp"
#include "runtime/sync.hpp"
#include "transport/backchannel.hpp"
#include "transport/record_stream.hpp"

namespace lnfs::transport {

struct TransportConfig {
  uint32_t max_fragment = 1u << 20;                    // 03 §3.2
  uint32_t max_request_size = (1u << 20) + (64 << 10);  // 01 §1.5: 1MiB + header slack
  int max_inflight_per_conn = 64;                      // v3 path; v4.1 uses session slots
  int max_connections = 4096;
  int per_peer_limit = 128;
};

// Binary per-peer key: the address without the port, v4 addresses v6-mapped so the two
// families never alias (plan doc 10 §2.4 — replaces inet_ntop + string keys).
struct IpKey {
  std::array<uint8_t, 16> b{};
  friend bool operator==(const IpKey&, const IpKey&) = default;
};
struct IpKeyHash {
  size_t operator()(const IpKey& k) const noexcept;
};

struct Peer {
  sockaddr_storage addr{};
  socklen_t len = 0;
  std::string to_string() const;
  // Key for per-peer connection limits (address without port).
  IpKey ip_key() const;
};

class ConnTracker {  // global + per-peer connection counting (design 03 §3.1)
 public:
  explicit ConnTracker(const TransportConfig& cfg) : cfg_(cfg) {}
  bool try_add(const Peer& p);
  void remove(const Peer& p);
  int count();

 private:
  // Sharded by peer key so parallel accept loops (per-reactor listeners, plan §2.3)
  // don't serialize on one lock; the global total is a lone atomic.
  static constexpr size_t kShards = 8;
  struct Shard {
    std::mutex mu;
    std::unordered_map<IpKey, int, IpKeyHash> per_peer;
  };
  Shard& shard_of(const IpKey& k) { return shards_[IpKeyHash{}(k) % kShards]; }

  const TransportConfig cfg_;
  std::atomic<int> total_{0};
  std::array<Shard, kShards> shards_;
};

struct ConnCtx {
  ConnCtx(int fd_, Peer peer_, rt::BufferPool& pool_, const TransportConfig& cfg)
      : fd(fd_),
        peer(std::move(peer_)),
        pool(pool_),
        rs(fd_, pool_, cfg.max_fragment, cfg.max_request_size),
        inflight(cfg.max_inflight_per_conn) {}

  int fd;
  Peer peer;
  rt::BufferPool& pool;
  RecordStream rs;
  rt::Semaphore inflight;
  rt::CancelSource cancel;
  int64_t live = 0;  // in-flight handler coroutines (same-reactor)
  rt::Event drained;
  bool send_failed = false;

  // Serialized reply send; on failure marks the connection for teardown.
  rt::Task<void> send(rt::SendBuf buf);

  // Backchannel send handle (plan doc 10 §5.2), created lazily on the connection's
  // reactor (CREATE_SESSION / BIND_CONN run as handlers here).  The v4 state layer
  // shares ownership; connection teardown detaches it.
  std::shared_ptr<CbChannel> cb_channel();
  // Read-loop upcall for an RPC REPLY record (a callback answer).  Public so tests
  // without a live read loop can inject replies.
  void route_cb_reply(rt::BufferChain rec);

  std::shared_ptr<CbChannel> cb;  // null until first cb_channel()
};

// Live-connection registry for `lightnfs-ctl conns` / `kill-conn` (plan doc 10 §4.2).
// connection_main registers on entry and removes before closing the fd, so kill() —
// which shuts the socket down under the registry mutex — can never touch a reused fd:
// the entry's presence proves the fd is still owned by that connection.
class ConnRegistry {
 public:
  static ConnRegistry& instance();

  uint64_t add(int fd, const Peer& peer);
  void remove(uint64_t id);

  struct Info {
    uint64_t id;
    std::string peer;
    int64_t age_s;
  };
  std::vector<Info> list();
  size_t count();
  // shutdown(SHUT_RDWR) wakes the connection's read loop; teardown then runs the
  // normal drain path.  Returns false when the id is gone already.
  bool kill(uint64_t id);

 private:
  struct Ent {
    int fd;
    Peer peer;
    std::chrono::steady_clock::time_point since;
  };
  std::mutex mu_;
  uint64_t next_id_ = 1;
  std::unordered_map<uint64_t, Ent> conns_;
};

// Owns the ConnCtx; runs until EOF/error, then drains and closes the fd.
rt::Task<void> connection_main(std::unique_ptr<ConnCtx> ctx, rpc::Dispatcher& disp,
                               ConnTracker* tracker);

}  // namespace lnfs::transport
