// The rebuildable data plane (plan 10 C1): activate → deactivate → activate over one
// running runtime, with the management plane (ctl) staying up throughout.  Loopback
// ephemeral ports only.  Checks that a deactivate converges live connections, joins
// the accept loops and the lease scanner, releases its metrics providers, and that the
// next activate serves again with a fresh epoch — the takeover shape the cluster
// controller (plan 10 C2) relies on.

#include "mini_test.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "obs/metrics.hpp"
#include "runtime/runtime.hpp"
#include "server/data_plane.hpp"
#include "transport/connection.hpp"

using namespace lnfs;
using namespace std::chrono_literals;

namespace {

int connect_loopback(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// One ctl round trip over the unix socket (like lightnfs-ctl): the reply text.
std::string ctl(const std::string& path, const std::string& command) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  path.copy(addr.sun_path, path.size());
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return "connect failed";
  }
  std::string line = command + "\n";
  (void)!write(fd, line.data(), line.size());
  shutdown(fd, SHUT_WR);
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return out;
}

server::CoreState make_core(uint64_t epoch) {
  auto exports = std::make_unique<core::ExportTable>();
  core::ExportConfig cfg;
  cfg.path = "/export/mem";
  cfg.fsid = 3;
  cfg.clients = {"127.0.0.0/8"};
  (void)exports->add(cfg, std::make_unique<backend::MemoryBackend>(3));
  std::array<std::byte, 16> key{std::byte{9}};
  server::CoreState core{std::move(exports), core::FileHandleCodec::from_key_only(key), epoch};
  core.key.bind(*core.exports);
  return core;
}

}  // namespace

TEST(DaemonLifecycle, ActivateDeactivateTwiceOverOneRuntime) {
  char tmpl[] = "/tmp/lnfs-dl-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  core::ServerConfig cfg;
  cfg.state_dir = dir;
  cfg.ctl_socket = dir + "/ctl.sock";
  cfg.port = 0;        // ephemeral loopback ports
  cfg.mount_port = 0;
  cfg.bind = "127.0.0.1";
  cfg.rpcbind = false;
  cfg.state_shards = 1;
  core::ClusterConfig cluster;

  rt::Runtime runtime({.reactors = 2, .offload_threads = 1});
  runtime.start();
  const size_t providers_before = obs::text_provider_count();
  const size_t conns_before = transport::ConnRegistry::instance().count();
  {
    server::Management mgmt = server::Management::start(cfg, runtime, {});
    ASSERT_TRUE(mgmt.ctl != nullptr);
    std::this_thread::sleep_for(50ms);  // ctl accept loop up
    EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=standby") != std::string::npos);
    EXPECT_STREQ(ctl(cfg.ctl_socket, "drc"), "not active\n");

    for (int round = 0; round < 2; ++round) {
      auto core = make_core(100 + static_cast<uint64_t>(round));
      auto plane = server::activate(cfg, cluster, core, runtime, mgmt);
      ASSERT_TRUE(plane.has_value());
      ASSERT_TRUE(plane->frontend.has_value());
      EXPECT_TRUE(plane->stack->nfs4.has_value());
      EXPECT_EQ(plane->stack->state.config().boot_epoch, 100u + static_cast<uint64_t>(round));
      EXPECT_EQ(obs::text_provider_count(), providers_before + 5);  // 4 groups + pool
      // ctl now addresses this plane; the epoch shows in the state dump.
      EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=active") != std::string::npos);
      EXPECT_TRUE(ctl(cfg.ctl_socket, "state")
                      .find("boot_epoch=" + std::to_string(100 + round)) != std::string::npos);
      EXPECT_TRUE(ctl(cfg.ctl_socket, "drc").find("inserts=") != std::string::npos);

      // A client that connects and stays silent: the deactivate has to shut it down.
      uint16_t port = plane->frontend->nfs->port();
      int cfd = connect_loopback(port);
      ASSERT_TRUE(cfd >= 0);
      auto deadline = std::chrono::steady_clock::now() + 2s;
      while (transport::ConnRegistry::instance().count() < conns_before + 1 &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      EXPECT_EQ(transport::ConnRegistry::instance().count(), conns_before + 1);

      auto t0 = std::chrono::steady_clock::now();
      bool converged = server::deactivate(*plane, cfg, mgmt, 100ms, 2s);
      auto took = std::chrono::steady_clock::now() - t0;
      EXPECT_TRUE(converged);
      EXPECT_TRUE(took < 2s);  // 100 ms grace + close; not the lease scanner's 1 s
      EXPECT_TRUE(plane->stack == nullptr);
      EXPECT_FALSE(plane->frontend.has_value());
      EXPECT_EQ(transport::ConnRegistry::instance().count(), conns_before);
      EXPECT_EQ(obs::text_provider_count(), providers_before);  // nothing leaked
      // The silent client was kicked: its read sees EOF/reset, not a hang.
      char b;
      EXPECT_TRUE(read(cfd, &b, 1) <= 0);
      close(cfd);
      // The port is closed for good (the listeners are gone, not just paused).
      int again = connect_loopback(port);
      EXPECT_TRUE(again < 0);
      if (again >= 0) close(again);
      // Management is still up and reports no data plane.
      EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=standby") != std::string::npos);
      EXPECT_STREQ(ctl(cfg.ctl_socket, "state"), "not active\n");
    }
    mgmt.stop();
  }
  runtime.stop_and_join();
  std::filesystem::remove_all(dir);
}

// A ctl command mid-flight pins the plane: detach waits for it instead of letting the
// command read a stack that is being destroyed.
TEST(DaemonLifecycle, DetachWaitsForPinnedCommands) {
  server::DataPlane plane{};
  server::DataPlaneSlot slot(&plane);
  EXPECT_EQ(slot.pins(), 0);
  {
    auto pin = slot.acquire();
    ASSERT_TRUE(static_cast<bool>(pin));
    EXPECT_EQ(slot.pins(), 1);
    EXPECT_FALSE(slot.detach(20ms));  // held: times out, plane pointer already cleared
    EXPECT_TRUE(slot.load() == nullptr);
    EXPECT_FALSE(static_cast<bool>(slot.acquire()));  // late comers see nothing
    EXPECT_EQ(slot.pins(), 1);
  }
  EXPECT_EQ(slot.pins(), 0);
  EXPECT_TRUE(slot.detach(20ms));
  // Re-attach, then a detach racing a pin released from another thread.
  slot.store(&plane);
  auto pin = slot.acquire();
  ASSERT_TRUE(static_cast<bool>(pin));
  std::thread releaser([p = std::move(pin)]() mutable {
    std::this_thread::sleep_for(30ms);
    server::PlaneRef gone = std::move(p);
  });
  auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(slot.detach(2s));
  EXPECT_TRUE(std::chrono::steady_clock::now() - t0 >= 25ms);
  releaser.join();
  EXPECT_EQ(slot.pins(), 0);
}
