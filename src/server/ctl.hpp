#pragma once
// Management endpoints (design 08 §8.3/8.6):
//  - CtlServer: line-oriented unix-socket admin interface for lightnfs-ctl
//    (ping / version / status / metrics / dump-errors / fdcache [flush] / drc [flush] /
//    clear-poison / state / expire-client <id> / conns / kill-conn <id> / loglevel /
//    reload / drain / grace-end, every command with an optional --json rendering);
//    owner-only socket, SO_PEERCRED-gated
//  - MetricsHttp: one-shot HTTP responder serving the Prometheus text exposition

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "runtime/cancel.hpp"
#include "runtime/runtime.hpp"

namespace lnfs::rpc {
class Drc;
}
namespace lnfs::state {
class StateMgr;
}

namespace lnfs::server {

// The data plane the ctl commands address (plan 10 A4): what exists only while the
// gateway serves — the export table, the DRC, the v4 state manager and the drain
// hook.  Attached by the frontend when the listeners are up and detached before they
// go away; a command arriving in between (a standby gateway, or a takeover in
// progress) answers "not active".  A null member inside an attached plane keeps its
// old meaning (v4 disabled, drc disabled, drain unavailable).
struct DataPlane {
  core::ExportTable* exports = nullptr;
  rpc::Drc* drc = nullptr;
  state::StateMgr* state = nullptr;  // v4 state table dump / forced client reclaim
  std::function<std::string()> drain;  // stop accepting new connections
  std::atomic<bool>* draining = nullptr;
};

// The switchable pointer to the data plane, with a pin count: a command holds a
// PlaneRef for as long as it uses the plane, and detach() (plan 10 C1) does not return
// while any pin is held, so a `state` dump mid-await never outlives the stack it reads.
class DataPlaneSlot;
class PlaneRef {
 public:
  PlaneRef() = default;
  PlaneRef(PlaneRef&& o) noexcept : slot_(o.slot_), plane_(o.plane_) {
    o.slot_ = nullptr;
    o.plane_ = nullptr;
  }
  PlaneRef& operator=(PlaneRef&&) = delete;
  ~PlaneRef();
  const DataPlane* get() const { return plane_; }
  const DataPlane* operator->() const { return plane_; }
  explicit operator bool() const { return plane_ != nullptr; }

 private:
  friend class DataPlaneSlot;
  PlaneRef(DataPlaneSlot* slot, const DataPlane* plane) : slot_(slot), plane_(plane) {}
  DataPlaneSlot* slot_ = nullptr;
  const DataPlane* plane_ = nullptr;
};

class DataPlaneSlot {
 public:
  explicit DataPlaneSlot(const DataPlane* plane = nullptr) : plane_(plane) {}
  void store(const DataPlane* plane) { plane_.store(plane, std::memory_order_release); }
  const DataPlane* load() const { return plane_.load(std::memory_order_acquire); }
  // Pins the current plane (null ref when none is attached).
  PlaneRef acquire();
  // Detaches, then waits until every pin is released; false if `timeout` passed
  // first (the caller must then not destroy the plane yet).
  bool detach(std::chrono::milliseconds timeout);
  int pins() const { return pins_.load(std::memory_order_acquire); }

 private:
  friend class PlaneRef;
  std::atomic<const DataPlane*> plane_;
  std::atomic<int> pins_{0};
};

struct CtlDeps {
  // Process-lifetime hooks (plan doc 10 §4.1); a null hook reports the feature unavailable.
  std::function<std::string()> reload;  // re-apply reloadable config, returns report
  std::chrono::steady_clock::time_point started{};
  // The switchable data plane; a null slot or a null pointer in it means not active.
  std::shared_ptr<DataPlaneSlot> plane;
  // `status` role text; empty = derived from the plane (active / standby).  The cluster
  // controller (plan 10 C2) supplies the finer states.
  std::function<std::string()> role;

  // Deps over a plane that stays attached for the deps' lifetime (single gateway,
  // tests).  `plane` must outlive the deps.
  static CtlDeps with_plane(const DataPlane* plane);
  // Pins the attached plane for the duration of one command (null when detached).
  PlaneRef acquire_plane() const { return plane ? plane->acquire() : PlaneRef{}; }
};

class CtlServer {
 public:
  static Result<std::unique_ptr<CtlServer>> create(const std::string& socket_path,
                                                   CtlDeps deps);
  ~CtlServer();
  rt::Task<void> run();
  // Spawns run() on `reactor` and remembers it, so wait_stopped() can join it.
  void start(rt::Reactor& reactor);
  void request_stop();
  // Blocks (not on a reactor) until a run() started via start() has exited: the
  // server may then be destroyed while the runtime keeps running (plan 10 C1).
  void wait_stopped();
  const std::string& path() const { return path_; }

  // Shared with MetricsHttp: text answer for one admin command.
  static std::string answer(const CtlDeps& deps, std::string_view command);
  // Commands that must run as coroutines (state table locks): falls back to answer().
  static rt::Task<std::string> answer_async(const CtlDeps& deps, std::string command);

 private:
  CtlServer(int fd, std::string path, CtlDeps deps)
      : fd_(fd), path_(std::move(path)), deps_(deps) {}
  rt::Task<void> serve(int cfd);

  int fd_;
  std::string path_;
  CtlDeps deps_;
  rt::CancelSource stop_;
  std::atomic<rt::Reactor*> run_reactor_{nullptr};
  bool started_ = false;
  std::promise<void> exited_;
  std::future<void> exited_future_ = exited_.get_future();
};

class MetricsHttp {
 public:
  // Binds to `bind_addr` (IPv4 or IPv6 literal; loopback by default at the config
  // layer, plan doc 10 §1.8).  `allow` is a CIDR allowlist checked per accepted
  // connection; empty = no filtering beyond the bind address.
  static Result<std::unique_ptr<MetricsHttp>> create(uint16_t port,
                                                     const std::string& bind_addr,
                                                     std::vector<core::Cidr> allow);
  ~MetricsHttp();
  rt::Task<void> run();
  void start(rt::Reactor& reactor);  // as CtlServer::start
  void request_stop();
  void wait_stopped();
  uint16_t port() const { return port_; }

 private:
  MetricsHttp(int fd, uint16_t port, std::vector<core::Cidr> allow)
      : fd_(fd), port_(port), allow_(std::move(allow)) {}
  rt::Task<void> serve(int cfd);
  bool allowed(const sockaddr_storage& peer) const;

  int fd_;
  uint16_t port_;
  std::vector<core::Cidr> allow_;
  rt::CancelSource stop_;
  std::atomic<rt::Reactor*> run_reactor_{nullptr};
  bool started_ = false;
  std::promise<void> exited_;
  std::future<void> exited_future_ = exited_.get_future();
};

}  // namespace lnfs::server
