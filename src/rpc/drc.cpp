#include "rpc/drc.hpp"

#include <cstring>

namespace lnfs::rpc {

Drc::Key Drc::Key::make(const sockaddr_storage& peer, uint32_t xid, uint32_t prog,
                        uint32_t vers, uint32_t proc, uint64_t args_hash) {
  Key k;
  if (peer.ss_family == AF_INET) {
    auto* a = reinterpret_cast<const sockaddr_in*>(&peer);
    k.peer_addr[10] = k.peer_addr[11] = 0xff;  // ::ffff:a.b.c.d
    std::memcpy(k.peer_addr.data() + 12, &a->sin_addr, 4);
    k.peer_port = ntohs(a->sin_port);
  } else if (peer.ss_family == AF_INET6) {
    auto* a = reinterpret_cast<const sockaddr_in6*>(&peer);
    std::memcpy(k.peer_addr.data(), &a->sin6_addr, 16);
    k.peer_port = ntohs(a->sin6_port);
  }
  k.xid = xid;
  k.prog = prog;
  k.vers = vers;
  k.proc = proc;
  k.args_hash = args_hash;
  return k;
}

size_t Drc::KeyHash::operator()(const Key& k) const noexcept {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  for (uint8_t c : k.peer_addr) mix(c);
  mix(k.peer_port);
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
    sh.bytes -= it->second.reply ? it->second.reply->size() : 0;
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
    it->second.reply = std::make_shared<const std::vector<std::byte>>(std::move(reply));
    it->second.done_at = std::chrono::steady_clock::now();
    sh.bytes += it->second.reply->size();
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
