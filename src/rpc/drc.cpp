#include "rpc/drc.hpp"

namespace lnfs::rpc {

size_t Drc::KeyHash::operator()(const Key& k) const noexcept {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  for (char c : k.peer) mix(static_cast<uint8_t>(c));
  mix(k.xid);
  mix(k.prog);
  mix(k.vers);
  mix(k.proc);
  mix(k.args_hash);
  return static_cast<size_t>(h);
}

void Drc::purge(Shard& sh) {
  auto now = std::chrono::steady_clock::now();
  while (!sh.completed.empty()) {
    const Key& oldest = sh.completed.front();
    auto it = sh.entries.find(oldest);
    if (it == sh.entries.end()) {  // aborted or already evicted
      sh.completed.pop_front();
      continue;
    }
    bool expired = now - it->second.done_at >= cfg_.ttl;
    bool over_memory = sh.bytes > cfg_.max_memory / kShards;
    if (!expired && !over_memory) break;
    sh.bytes -= it->second.reply.size();
    sh.entries.erase(it);
    sh.completed.pop_front();
    evictions_.fetch_add(1, std::memory_order_relaxed);
  }
}

rt::Task<Drc::Claim> Drc::begin(const Key& key) {
  Shard& sh = shard_of(key);
  auto lk = co_await sh.mu.lock();
  for (;;) {
    purge(sh);
    auto it = sh.entries.find(key);
    if (it == sh.entries.end()) {
      sh.entries.emplace(key, Entry{});
      inserts_.fetch_add(1, std::memory_order_relaxed);
      co_return Claim{.owner = true};
    }
    if (it->second.done) {
      replays_.fetch_add(1, std::memory_order_relaxed);
      co_return Claim{.owner = false, .cached = it->second.reply};
    }
    // In progress: wait for the original execution, then re-check (it may complete,
    // abort, or expire — the loop classifies whichever happened).
    waits_.fetch_add(1, std::memory_order_relaxed);
    co_await sh.cv.wait(sh.mu, lk);
  }
}

rt::Task<void> Drc::complete(const Key& key, std::vector<std::byte> reply) {
  Shard& sh = shard_of(key);
  auto lk = co_await sh.mu.lock();
  auto it = sh.entries.find(key);
  if (it != sh.entries.end() && !it->second.done) {
    it->second.done = true;
    it->second.reply = std::move(reply);
    it->second.done_at = std::chrono::steady_clock::now();
    sh.bytes += it->second.reply.size();
    sh.completed.push_back(key);
    purge(sh);
  }
  sh.cv.notify_all();
}

rt::Task<void> Drc::abort(const Key& key) {
  Shard& sh = shard_of(key);
  auto lk = co_await sh.mu.lock();
  auto it = sh.entries.find(key);
  if (it != sh.entries.end() && !it->second.done) sh.entries.erase(it);
  sh.cv.notify_all();
}

Drc::Stats Drc::stats() const {
  Stats out;
  out.inserts = inserts_.load(std::memory_order_relaxed);
  out.replays = replays_.load(std::memory_order_relaxed);
  out.waits = waits_.load(std::memory_order_relaxed);
  out.evictions = evictions_.load(std::memory_order_relaxed);
  for (const auto& sh : shards_) {
    out.entries += sh.entries.size();  // racy reads: stats are advisory
    out.bytes += sh.bytes;
  }
  return out;
}

}  // namespace lnfs::rpc
