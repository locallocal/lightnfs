#include "backend/fault.hpp"

#include <atomic>
#include <cstdlib>

namespace lnfs::backend::fault {
namespace {

struct State {
  std::atomic<int> left[static_cast<int>(Kind::kCount)];
  std::atomic<int> slow_ms;

  State() {
    static constexpr const char* kEnv[] = {
        "LNFS_FAULT_FSYNC_EIO",   "LNFS_FAULT_WRITE_ENOSPC", "LNFS_FAULT_WRITE_EDQUOT",
        "LNFS_FAULT_READ_EIO",    "LNFS_FAULT_SHORT_WRITE",  "LNFS_FAULT_SLOW_IO",
    };
    for (int i = 0; i < static_cast<int>(Kind::kCount); ++i) {
      const char* v = std::getenv(kEnv[i]);
      left[i].store(v ? std::atoi(v) : 0, std::memory_order_relaxed);
    }
    const char* ms = std::getenv("LNFS_FAULT_SLOW_MS");
    slow_ms.store(ms ? std::atoi(ms) : 50, std::memory_order_relaxed);
  }
};

State& state() {
  static State s;
  return s;
}

}  // namespace

bool take(Kind k) {
  auto& left = state().left[static_cast<int>(k)];
  int cur = left.load(std::memory_order_relaxed);
  while (cur > 0) {
    if (left.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) return true;
  }
  return false;
}

void arm(Kind k, int count) {
  state().left[static_cast<int>(k)].store(count, std::memory_order_relaxed);
}

void clear() {
  for (int i = 0; i < static_cast<int>(Kind::kCount); ++i)
    state().left[i].store(0, std::memory_order_relaxed);
}

int slow_ms() { return state().slow_ms.load(std::memory_order_relaxed); }
void set_slow_ms(int ms) { state().slow_ms.store(ms, std::memory_order_relaxed); }

}  // namespace lnfs::backend::fault
