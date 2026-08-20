#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "mountd/mount3.hpp"
#include "nfsv3/engine.hpp"
#include "runtime/runtime.hpp"
#include "server/rpcbind.hpp"
#include "transport/listener.hpp"
#include "util/log.hpp"

namespace {

volatile std::sig_atomic_t stopping = 0;
void on_signal(int) { stopping = 1; }

lnfs::Result<void> run_backend_hook(lnfs::rt::Reactor& reactor,
                                    lnfs::rt::Task<lnfs::Result<void>> task) {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  lnfs::Result<void> result;
  lnfs::rt::spawn(
      [](lnfs::rt::Task<lnfs::Result<void>> work, std::mutex* mu,
         std::condition_variable* cv, bool* done,
         lnfs::Result<void>* result) -> lnfs::rt::Task<void> {
        *result = co_await std::move(work);
        {
          std::lock_guard lock(*mu);
          *done = true;
        }
        cv->notify_one();
      }(std::move(task), &mu, &cv, &done, &result),
      reactor);
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return done; });
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "/etc/lightnfs/lightnfs.toml";
  bool check_only = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--check-config") check_only = true;
    else if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
    else {
      std::fprintf(stderr, "usage: %s [--config FILE] [--check-config]\n", argv[0]);
      return 2;
    }
  }

  auto config_result = lnfs::core::load_config(config_path);
  if (!config_result) {
    LNFS_ERROR("cannot load config {}: {}", config_path,
               lnfs::errno_name(config_result.error()));
    return 1;
  }
  auto validation = lnfs::core::validate_config(*config_result);
  if (!validation) {
    LNFS_ERROR("invalid config {}: {}", config_path, lnfs::errno_name(validation.error()));
    return 1;
  }
  if (check_only) {
    std::printf("configuration is valid\n");
    return 0;
  }

  lnfs::core::ServerConfig server_cfg = config_result->server;
  auto exports_result = lnfs::core::ExportTable::build(std::move(*config_result));
  if (!exports_result) {
    LNFS_ERROR("cannot initialize exports: {}", lnfs::errno_name(exports_result.error()));
    return 1;
  }
  auto key = lnfs::core::FileHandleCodec::load_or_create(server_cfg.state_dir);
  if (!key) {
    LNFS_ERROR("cannot load file-handle key: {}", lnfs::errno_name(key.error()));
    return 1;
  }
  auto exports = std::move(*exports_result);
  key->bind(*exports);

  lnfs::rt::Runtime runtime({.reactors = server_cfg.reactors,
                             .offload_threads = server_cfg.offload_threads});
  runtime.start();
  for (const auto& entry : exports->entries()) {
    auto started = run_backend_hook(runtime.reactor(0), entry->backend->start());
    if (!started) {
      LNFS_ERROR("backend {} failed to start: {}", entry->path,
                 lnfs::errno_name(started.error()));
      runtime.stop_and_join();
      return 1;
    }
  }

  lnfs::rpc::Dispatcher dispatcher;
  lnfs::core::ObjLockRegistry locks;
  lnfs::nfsv3::Engine nfs(*exports, *key, locks);
  lnfs::mountd::Mount3 mount(*exports, *key);
  nfs.register_with(dispatcher);
  mount.register_with(dispatcher);

  lnfs::transport::TransportConfig transport_cfg;
  transport_cfg.max_request_size = server_cfg.max_request_size;
  transport_cfg.max_inflight_per_conn = server_cfg.inflight_per_conn;
  transport_cfg.max_connections = server_cfg.max_connections;
  auto nfs_listener = lnfs::transport::Listener::create(server_cfg.port, transport_cfg,
                                                         dispatcher, runtime);
  auto mount_listener = lnfs::transport::Listener::create(server_cfg.mount_port, transport_cfg,
                                                           dispatcher, runtime);
  if (!nfs_listener || !mount_listener) {
    LNFS_ERROR("cannot create listeners: nfs={} mount={}",
               nfs_listener ? "ok" : lnfs::errno_name(nfs_listener.error()),
               mount_listener ? "ok" : lnfs::errno_name(mount_listener.error()));
    runtime.stop_and_join();
    return 1;
  }
  lnfs::rt::spawn((*nfs_listener)->run(), runtime.reactor(0));
  lnfs::rt::spawn((*mount_listener)->run(), runtime.reactor(0));

  if (server_cfg.rpcbind) {
    auto nfs_reg = lnfs::server::rpcbind_set(lnfs::nfsv3::kProgram, lnfs::nfsv3::kVersion,
                                              (*nfs_listener)->port());
    auto mount_reg = lnfs::server::rpcbind_set(lnfs::mountd::kProgram, lnfs::mountd::kVersion,
                                                (*mount_listener)->port());
    if (!nfs_reg || !mount_reg)
      LNFS_WARN("rpcbind registration unavailable; use explicit port/mountport options");
  }
  LNFS_INFO("lightnfs ready: nfs_port={} mount_port={} exports={}",
            (*nfs_listener)->port(), (*mount_listener)->port(), exports->entries().size());

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  (*nfs_listener)->request_stop();
  (*mount_listener)->request_stop();
  if (server_cfg.rpcbind) {
    (void)lnfs::server::rpcbind_unset(lnfs::nfsv3::kProgram, lnfs::nfsv3::kVersion);
    (void)lnfs::server::rpcbind_unset(lnfs::mountd::kProgram, lnfs::mountd::kVersion);
  }
  for (const auto& entry : exports->entries())
    (void)run_backend_hook(runtime.reactor(0), entry->backend->stop());
  runtime.stop_and_join();
  LNFS_INFO("lightnfs stopped");
  return 0;
}
