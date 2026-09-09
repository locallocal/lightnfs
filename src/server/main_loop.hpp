#pragma once
// The daemon's main-loop thread (design 01 §1.4, plan 12 C2): where the data plane is
// activated and torn down, exports are applied and backends start / stop.  Other
// threads (the cluster controllers' tick threads, the ctl reactor) post work here;
// `call` posts and waits, so a ctl `reload` runs its file IO and export changes on
// this thread rather than on the reactor that received the command.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lnfs::server {

class MainLoop {
 public:
  void post(std::function<void()> fn) {
    {
      std::lock_guard lock(mu_);
      queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
  }

  // Runs `fn` on the loop thread and hands back its result; nullopt when the loop did
  // not get to it within `timeout` (not running yet, or shutting down — the closure
  // may still run later and is then discarded).  From the loop thread itself `fn`
  // runs inline: waiting there would wait on itself.
  std::optional<std::string> call(std::function<std::string()> fn,
                                  std::chrono::milliseconds timeout) {
    if (std::this_thread::get_id() == loop_thread_.load(std::memory_order_acquire)) return fn();
    auto done = std::make_shared<std::promise<std::string>>();
    auto result = done->get_future();
    post([done, fn = std::move(fn)] { done->set_value(fn()); });
    if (result.wait_for(timeout) != std::future_status::ready) return std::nullopt;
    return result.get();
  }

  // Serves posted work on the calling thread until `stopping()`; between items polls
  // `take_reload_request()` (SIGHUP) and runs `on_reload` here.
  void run(const std::function<bool()>& stopping, const std::function<bool()>& take_reload_request,
           const std::function<void()>& on_reload) {
    loop_thread_.store(std::this_thread::get_id(), std::memory_order_release);
    while (!stopping()) {
      if (take_reload_request()) on_reload();
      std::function<void()> work;
      {
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !queue_.empty(); });
        if (!queue_.empty()) {
          work = std::move(queue_.front());
          queue_.pop_front();
        }
      }
      if (work) work();
    }
  }

  // Runs whatever is still queued, on the calling thread (shutdown).
  void drain() {
    loop_thread_.store(std::this_thread::get_id(), std::memory_order_release);
    for (;;) {
      std::function<void()> work;
      {
        std::lock_guard lock(mu_);
        if (queue_.empty()) return;
        work = std::move(queue_.front());
        queue_.pop_front();
      }
      work();
    }
  }

  bool on_loop_thread() const {
    return std::this_thread::get_id() == loop_thread_.load(std::memory_order_acquire);
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::atomic<std::thread::id> loop_thread_{};
};

}  // namespace lnfs::server
