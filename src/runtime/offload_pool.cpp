#include "runtime/offload_pool.hpp"

#include <algorithm>

namespace lnfs::rt {

OffloadPool::OffloadPool(Config cfg) {
  int total = cfg.threads > 0 ? cfg.threads : 1;
  int heavy = cfg.heavy_threads > 0 ? cfg.heavy_threads : std::max(1, total / 4);
  // The light group always keeps at least one worker; a 1-thread pool has no separate
  // heavy group and heavy jobs share the light workers (see group_for()).
  if (heavy >= total) heavy = total - 1;
  int light = total - heavy;

  auto init_group = [&](Group& g, int workers) {
    g.queue_cap = cfg.queue_cap > 0 ? cfg.queue_cap : SIZE_MAX;
    g.shards.reserve(workers);
    for (int i = 0; i < workers; ++i) g.shards.push_back(std::make_unique<Shard>());
  };
  init_group(groups_[0], light);
  init_group(groups_[1], heavy);

  threads_.reserve(total);
  for (int i = 0; i < light; ++i)
    threads_.emplace_back([this, i] { worker(groups_[0], static_cast<size_t>(i)); });
  for (int i = 0; i < heavy; ++i)
    threads_.emplace_back([this, i] { worker(groups_[1], static_cast<size_t>(i)); });
}

OffloadPool::~OffloadPool() { stop_and_join(); }

void OffloadPool::stop_and_join() {
  if (stopping_.exchange(true)) return;
  for (auto& g : groups_) {
    // Bypass the admission cap so parked jobs still run during the drain, then hand
    // every worker one exit permit (consumed only when its scan comes up empty).
    {
      std::lock_guard lk(g.overflow_mu);
      while (!g.overflow.empty()) {
        MoveOnlyFn job = std::move(g.overflow.front());
        g.overflow.pop_front();
        size_t s = g.rr.fetch_add(1, std::memory_order_relaxed) % g.shards.size();
        {
          std::lock_guard slk(g.shards[s]->mu);
          g.shards[s]->q.push_back(std::move(job));
        }
        g.depth.fetch_add(1, std::memory_order_relaxed);
        g.sem.release();
      }
    }
    g.sem.release(static_cast<std::ptrdiff_t>(g.shards.size()));
  }
  for (auto& t : threads_) t.join();
  threads_.clear();
}

void OffloadPool::enqueue(Group& g, MoveOnlyFn job) {
  size_t s = g.rr.fetch_add(1, std::memory_order_relaxed) % g.shards.size();
  {
    std::lock_guard lk(g.shards[s]->mu);
    g.shards[s]->q.push_back(std::move(job));
  }
  g.depth.fetch_add(1, std::memory_order_relaxed);
  g.sem.release();
}

void OffloadPool::submit(MoveOnlyFn job, OffloadClass cls) {
  Group& g = group_for(cls);
  g.submitted.fetch_add(1, std::memory_order_relaxed);
  // Admission cap (plan doc 10 §2.5): keep the runnable queues bounded; excess jobs
  // wait in overflow and are promoted as workers drain. Bypassed during shutdown so
  // the drain terminates.
  if (g.depth.load(std::memory_order_relaxed) >= g.queue_cap &&
      !stopping_.load(std::memory_order_relaxed)) {
    g.deferred.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(g.overflow_mu);
    g.overflow.push_back(std::move(job));
    return;
  }
  enqueue(g, std::move(job));
}

bool OffloadPool::try_pop(Group& g, size_t start, MoveOnlyFn& out) {
  const size_t n = g.shards.size();
  for (size_t k = 0; k < n; ++k) {
    Shard& s = *g.shards[(start + k) % n];
    std::lock_guard lk(s.mu);
    if (!s.q.empty()) {
      out = std::move(s.q.front());
      s.q.pop_front();
      return true;
    }
  }
  return false;
}

void OffloadPool::refill_from_overflow(Group& g) {
  if (g.depth.load(std::memory_order_relaxed) >= g.queue_cap) return;
  MoveOnlyFn job;
  {
    std::lock_guard lk(g.overflow_mu);
    if (g.overflow.empty()) return;
    job = std::move(g.overflow.front());
    g.overflow.pop_front();
  }
  enqueue(g, std::move(job));
}

void OffloadPool::worker(Group& g, size_t idx) {
  for (;;) {
    g.sem.acquire();
    MoveOnlyFn job;
    // The semaphore counts queued jobs plus one exit permit per worker at shutdown.
    // A permit whose job was raced away by a neighbor's steal scan is put back.
    if (!try_pop(g, idx, job)) {
      if (stopping_.load(std::memory_order_acquire)) return;
      g.sem.release();
      std::this_thread::yield();
      continue;
    }
    g.depth.fetch_sub(1, std::memory_order_relaxed);
    job();
    g.completed.fetch_add(1, std::memory_order_relaxed);
    refill_from_overflow(g);
  }
}

OffloadPool::Stats OffloadPool::stats() const {
  Stats out;
  for (int c = 0; c < kOffloadClasses; ++c) {
    const Group& g = groups_[c];
    out.submitted[c] = g.submitted.load(std::memory_order_relaxed);
    out.completed[c] = g.completed.load(std::memory_order_relaxed);
    out.deferred[c] = g.deferred.load(std::memory_order_relaxed);
    out.depth[c] = g.depth.load(std::memory_order_relaxed);
    std::lock_guard lk(g.overflow_mu);
    out.depth[c] += g.overflow.size();
  }
  return out;
}

}  // namespace lnfs::rt
