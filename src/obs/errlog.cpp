#include "obs/errlog.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <format>
#include <mutex>

namespace lnfs::obs {
namespace {

struct Entry {
  time_t when = 0;
  uint32_t xid = 0, status = 0;
  char what[20] = {};
  char peer[48] = {};
};

constexpr size_t kRing = 64;
std::mutex g_mu;
std::array<Entry, kRing> g_ring;
size_t g_next = 0;
uint64_t g_total = 0;

}  // namespace

void record_error_reply(std::string_view peer, std::string_view what, uint32_t xid,
                        uint32_t status) {
  std::lock_guard lock(g_mu);
  Entry& e = g_ring[g_next];
  g_next = (g_next + 1) % kRing;
  ++g_total;
  e.when = ::time(nullptr);
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
  for (size_t i = 0; i < kRing; ++i) {
    const Entry& e = g_ring[(g_next + i) % kRing];  // oldest first
    if (e.when == 0) continue;
    out += std::format("ts={} peer={} proc={} xid={:#x} status={}\n", e.when, e.peer,
                       e.what, e.xid, e.status);
  }
  return out;
}

}  // namespace lnfs::obs
