#pragma once
// Management endpoints (design 08 §8.3/8.6, minimal phase-2 versions):
//  - CtlServer: line-oriented unix-socket admin interface for lightnfs-ctl
//    (ping / metrics / dump-errors / fdcache / drc / state / expire-client <id>)
//  - MetricsHttp: one-shot HTTP responder serving the Prometheus text exposition

#include <memory>
#include <string>

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

struct CtlDeps {
  core::ExportTable* exports = nullptr;
  rpc::Drc* drc = nullptr;
  state::StateMgr* state = nullptr;  // v4 state table dump / forced client reclaim
};

class CtlServer {
 public:
  static Result<std::unique_ptr<CtlServer>> create(const std::string& socket_path,
                                                   CtlDeps deps);
  ~CtlServer();
  rt::Task<void> run();
  void request_stop();
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
};

class MetricsHttp {
 public:
  static Result<std::unique_ptr<MetricsHttp>> create(uint16_t port);
  ~MetricsHttp();
  rt::Task<void> run();
  void request_stop();
  uint16_t port() const { return port_; }

 private:
  MetricsHttp(int fd, uint16_t port) : fd_(fd), port_(port) {}
  rt::Task<void> serve(int cfd);

  int fd_;
  uint16_t port_;
  rt::CancelSource stop_;
  std::atomic<rt::Reactor*> run_reactor_{nullptr};
};

}  // namespace lnfs::server
