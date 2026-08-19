#include "runtime/offload_pool.hpp"

namespace lnfs::rt {

OffloadPool::OffloadPool(int threads) {
  threads_.reserve(threads);
  for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

OffloadPool::~OffloadPool() { stop_and_join(); }

void OffloadPool::stop_and_join() {
  {
    std::lock_guard lk(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
  threads_.clear();
}

void OffloadPool::submit(MoveOnlyFn job) {
  {
    std::lock_guard lk(mu_);
    q_.push_back(std::move(job));
  }
  cv_.notify_one();
}

size_t OffloadPool::queue_depth() {
  std::lock_guard lk(mu_);
  return q_.size();
}

void OffloadPool::worker() {
  for (;;) {
    MoveOnlyFn job;
    {
      std::unique_lock lk(mu_);
      cv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
      if (q_.empty()) return;  // stopping: drain first, then exit
      job = std::move(q_.front());
      q_.pop_front();
    }
    job();
  }
}

}  // namespace lnfs::rt
