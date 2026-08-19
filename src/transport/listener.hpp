#pragma once
// Accept loop (design 03 §3.1): accepts on one reactor, assigns connections round-robin
// across the runtime's reactors, enforces global/per-peer connection limits.

#include <memory>

#include "runtime/runtime.hpp"
#include "transport/connection.hpp"

namespace lnfs::transport {

class Listener {
 public:
  // Binds and listens; port 0 picks an ephemeral port (tests).
  static Result<std::unique_ptr<Listener>> create(uint16_t port, TransportConfig cfg,
                                                  rpc::Dispatcher& disp, rt::Runtime& rt);
  ~Listener();

  uint16_t port() const { return port_; }
  ConnTracker& tracker() { return tracker_; }

  // The accept loop; spawn this on a reactor. Exits when request_stop() is called.
  rt::Task<void> run();
  // Thread-safe: cancels the in-flight accept on the reactor run() lives on.
  void request_stop();

 private:
  Listener(int fd, uint16_t port, TransportConfig cfg, rpc::Dispatcher& disp, rt::Runtime& rt)
      : fd_(fd), port_(port), cfg_(cfg), disp_(disp), rt_(rt), tracker_(cfg) {}

  int fd_;
  uint16_t port_;
  TransportConfig cfg_;
  rpc::Dispatcher& disp_;
  rt::Runtime& rt_;
  ConnTracker tracker_;
  rt::CancelSource stop_;
  std::atomic<rt::Reactor*> run_reactor_{nullptr};
  // Each reactor needs its own pool eventually; one shared thread-safe pool for now.
  rt::BufferPool pool_;
};

}  // namespace lnfs::transport
