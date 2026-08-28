#include "obs/errlog.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

namespace lnfs::obs {
namespace {

struct Entry {
  int64_t when_ms = 0;  // wall clock, milliseconds since the epoch
  uint32_t xid = 0, status = 0;
  // Sized for the longest v4 op name ("BIND_CONN_TO_SESSION"); the old 20-byte field
  // truncated exactly that class of name (plan doc 10 §3.7).
  char what[32] = {};
  char peer[48] = {};
};

std::mutex g_mu;
std::vector<Entry> g_ring(64);
size_t g_next = 0;
uint64_t g_total = 0;

}  // namespace

void set_error_ring_capacity(size_t entries) {
  std::lock_guard lock(g_mu);
  g_ring.assign(std::max<size_t>(entries, 1), Entry{});
  g_next = 0;
}

void record_error_reply(std::string_view peer, std::string_view what, uint32_t xid,
                        uint32_t status) {
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  std::lock_guard lock(g_mu);
  Entry& e = g_ring[g_next];
  g_next = (g_next + 1) % g_ring.size();
  ++g_total;
  e.when_ms = now_ms;
  e.xid = xid;
  e.status = status;
  size_t w = std::min(what.size(), sizeof(e.what) - 1);
  what.copy(e.what, w);
  e.what[w] = '\0';
  size_t n = std::min(peer.size(), sizeof(e.peer) - 1);
  peer.copy(e.peer, n);
  e.peer[n] = '\0';
}

std::string dump_error_replies() {
  std::lock_guard lock(g_mu);
  std::string out = std::format("total_errors={}\n", g_total);
  for (size_t i = 0; i < g_ring.size(); ++i) {
    const Entry& e = g_ring[(g_next + i) % g_ring.size()];  // oldest first
    if (e.when_ms == 0) continue;
    out += std::format("ts={}.{:03} peer={} proc={} xid={:#x} status={}\n",
                       e.when_ms / 1000, e.when_ms % 1000, e.peer, e.what, e.xid,
                       e.status);
  }
  return out;
}

}  // namespace lnfs::obs
