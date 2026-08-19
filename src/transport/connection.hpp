#pragma once
// Per-connection context and main loop (design 03 §3.3, 01 §1.5).
//  - one connection_main coroutine per accepted socket, pinned to one reactor
//  - request pipelining: each parsed record spawns an independent handler coroutine
//  - backpressure: per-conn inflight Semaphore gates further record parsing (TCP pushback)
//  - teardown: cancel token -> best-effort ring cancel -> drain in-flight handlers

#include <netinet/in.h>

#include <map>
#include <memory>
#include <string>

#include "rpc/dispatch.hpp"
#include "runtime/buffer.hpp"
#include "runtime/cancel.hpp"
#include "runtime/sync.hpp"
#include "transport/record_stream.hpp"

namespace lnfs::transport {

struct TransportConfig {
  uint32_t max_fragment = 1u << 20;                    // 03 §3.2
  uint32_t max_request_size = (1u << 20) + (64 << 10);  // 01 §1.5: 1MiB + header slack
  int max_inflight_per_conn = 64;                      // v3 path; v4.1 uses session slots
  int max_connections = 4096;
  int per_peer_limit = 128;
};

struct Peer {
  sockaddr_storage addr{};
  socklen_t len = 0;
  std::string to_string() const;
  // Key for per-peer connection limits (address without port).
  std::string ip_key() const;
};

class ConnTracker {  // global + per-peer connection counting (design 03 §3.1)
 public:
  explicit ConnTracker(const TransportConfig& cfg) : cfg_(cfg) {}
  bool try_add(const Peer& p);
  void remove(const Peer& p);
  int count();

 private:
  const TransportConfig cfg_;
  std::mutex mu_;
  int total_ = 0;
  std::map<std::string, int> per_peer_;
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
};

// Owns the ConnCtx; runs until EOF/error, then drains and closes the fd.
rt::Task<void> connection_main(std::unique_ptr<ConnCtx> ctx, rpc::Dispatcher& disp,
                               ConnTracker* tracker);

}  // namespace lnfs::transport
