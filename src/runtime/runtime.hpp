#pragma once
// Runtime: owns N reactors (each with its RingOps), their threads, and the offload pool
// (design 01 §1.3/§1.4: --reactors N, --offload M; connections round-robin over reactors).
// Ring backend: "auto" probes io_uring and falls back to epoll (design risk item: kernel
// feature differences — the probe result is logged at startup).

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "runtime/offload_pool.hpp"
#include "runtime/reactor.hpp"

namespace lnfs::rt {

class Runtime {
 public:
  struct Config {
    int reactors = 0;  // 0 = hardware concurrency
    int offload_threads = 8;
    int offload_heavy_threads = 0;      // 0 = max(1, offload_threads/4) (plan §2.5)
    size_t offload_queue_cap = 4096;    // per-class admission cap (plan §2.5)
    std::string ring = "auto";  // auto | uring | epoll
    unsigned ring_entries = 1024;
    unsigned ring_cq_entries = 0;  // 0 = 8 × ring_entries (plan §2.3)
    bool ring_sqpoll = false;      // io_uring SQPOLL submission (design 02 §2.63)
  };

  explicit Runtime(Config cfg);
  ~Runtime();

  size_t reactor_count() const { return reactors_.size(); }
  Reactor& reactor(size_t i) { return *reactors_[i]; }
  // Round-robin assignment for new connections (design 03 §3.1).
  Reactor& next() {
    return *reactors_[rr_.fetch_add(1, std::memory_order_relaxed) % reactors_.size()];
  }
  OffloadPool& offload() { return *offload_; }
  const std::string& ring_kind() const { return ring_kind_; }

  void start();          // one thread per reactor
  void stop_and_join();  // stop all reactors and join their threads

 private:
  std::vector<std::unique_ptr<RingOps>> rings_;
  std::vector<std::unique_ptr<Reactor>> reactors_;
  std::unique_ptr<OffloadPool> offload_;
  std::vector<std::thread> threads_;
  std::atomic<uint64_t> rr_{0};
  std::string ring_kind_;
};

}  // namespace lnfs::rt
