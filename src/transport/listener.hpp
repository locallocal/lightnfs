#pragma once
// Accept path (design 03 §3.1, reworked per plan doc 10 §2.3): one SO_REUSEPORT
// listening socket per reactor, each with its own accept loop pinned to that reactor.
// The kernel load-balances incoming connections across the sockets, so there is no
// single accept serialization point and no cross-reactor handoff — a connection is
// served where it was accepted. Global/per-peer connection limits are shared.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/runtime.hpp"
#include "transport/connection.hpp"

namespace lnfs::transport {

class Listener {
 public:
  // Binds one socket per reactor; port 0 picks an ephemeral port (tests) which the
  // remaining sockets then share via SO_REUSEPORT.  bind_addr: IPv4/IPv6 literal;
  // empty = all interfaces, dual-stack (plan doc 10 §4.4).
  static Result<std::unique_ptr<Listener>> create(uint16_t port, TransportConfig cfg,
                                                  rpc::Dispatcher& disp, rt::Runtime& rt,
                                                  const std::string& bind_addr = "");
  ~Listener();

  uint16_t port() const { return port_; }
  ConnTracker& tracker() { return tracker_; }
  rt::BufferPool& pool() { return pool_; }  // watermark metrics (plan doc 10 §3.5)

  // Spawns the accept loops (one per reactor). They exit when request_stop() is called.
  void start();
  // Thread-safe: cancels every in-flight accept on its owning reactor.
  void request_stop();
  // Blocks (any thread but a reactor's) until every accept loop has exited, so the
  // listener — and the dispatcher it serves — may be destroyed while the runtime keeps
  // running (a rebuilt protocol stack, plan 10 C1).
  void wait_stopped();

 private:
  Listener(std::vector<int> fds, uint16_t port, TransportConfig cfg, rpc::Dispatcher& disp,
           rt::Runtime& rt)
      : fds_(std::move(fds)), port_(port), cfg_(cfg), disp_(disp), rt_(rt), tracker_(cfg) {}

  rt::Task<void> run_one(size_t idx);

  std::vector<int> fds_;  // fds_[i] is accepted on rt_.reactor(i)
  uint16_t port_;
  TransportConfig cfg_;
  rpc::Dispatcher& disp_;
  rt::Runtime& rt_;
  ConnTracker tracker_;
  rt::CancelSource stop_;
  std::mutex loops_mu_;
  std::condition_variable loops_cv_;
  size_t loops_running_ = 0;
  // Each reactor needs its own pool eventually; one shared thread-safe pool for now
  // (per-thread magazines make the sharing cheap, see runtime/buffer.hpp).
  rt::BufferPool pool_;
};

}  // namespace lnfs::transport
