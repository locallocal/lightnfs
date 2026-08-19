#include "runtime/runtime.hpp"

#include "runtime/epoll_ring.hpp"
#include "runtime/uring_ring.hpp"
#include "util/log.hpp"

namespace lnfs::rt {

Runtime::Runtime(Config cfg) {
  int n = cfg.reactors > 0 ? cfg.reactors
                           : static_cast<int>(std::thread::hardware_concurrency());
  if (n <= 0) n = 1;

#ifdef LNFS_DEFAULT_RING_EPOLL
  if (cfg.ring == "auto") cfg.ring = "epoll";
#endif

  for (int i = 0; i < n; ++i) {
    std::unique_ptr<RingOps> ring;
    if (cfg.ring == "uring" || cfg.ring == "auto") {
      auto r = UringRing::create(cfg.ring_entries);
      if (r) {
        ring = std::move(*r);
        ring_kind_ = "uring";
      } else if (cfg.ring == "uring") {
        LNFS_ERROR("io_uring unavailable (errno={}) and ring=uring forced", raw(r.error()));
        std::abort();
      }
    }
    if (!ring) {
      ring = std::make_unique<EpollRing>();
      ring_kind_ = "epoll";
    }
    rings_.push_back(std::move(ring));
    reactors_.push_back(std::make_unique<Reactor>(*rings_.back()));
  }
  offload_ = std::make_unique<OffloadPool>(cfg.offload_threads);
  for (auto& r : reactors_) r->set_offload_pool(offload_.get());
  LNFS_INFO("runtime: {} reactors, ring={}, offload_threads={}", reactors_.size(), ring_kind_,
            cfg.offload_threads);
}

Runtime::~Runtime() { stop_and_join(); }

void Runtime::start() {
  threads_.reserve(reactors_.size());
  for (auto& r : reactors_) threads_.emplace_back([rp = r.get()] { rp->run(); });
}

void Runtime::stop_and_join() {
  for (auto& r : reactors_) r->stop();
  for (auto& t : threads_) t.join();
  threads_.clear();
}

}  // namespace lnfs::rt
