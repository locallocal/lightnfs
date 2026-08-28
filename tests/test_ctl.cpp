// Management-plane hardening (plan doc 10 §1.8): ctl socket permissions + fragmented
// command framing, metrics bind address + CIDR allowlist, and the new config keys.

#include "mini_test.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "backend/memory.hpp"
#include "core/config.hpp"
#include "runtime/runtime.hpp"
#include "server/ctl.hpp"
#include "transport/connection.hpp"
#include "util/log.hpp"

using namespace lnfs;

TEST(Ctl, FragmentedCommandFramingAndSocketPerms) {
  char tmpl[] = "/tmp/lnfs-ctl-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string sock = dir + "/ctl.sock";
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  auto ctl = server::CtlServer::create(sock, {});
  ASSERT_TRUE(ctl.has_value());

  // Owner-only permissions regardless of umask.
  struct stat st {};
  ASSERT_TRUE(::stat(sock.c_str(), &st) == 0);
  EXPECT_EQ(static_cast<unsigned>(st.st_mode & 0777), 0600u);

  rt::spawn((*ctl)->run(), runtime.reactor(0));

  // A command split across two sends used to be truncated at the first recv.
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_TRUE(fd >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  sock.copy(addr.sun_path, sock.size());
  ASSERT_TRUE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  ASSERT_TRUE(::write(fd, "pi", 2) == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(::write(fd, "ng\n", 3) == 3);
  std::string got;
  char buf[64];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) got.append(buf, static_cast<size_t>(n));
  ::close(fd);
  EXPECT_STREQ(got, "pong\n");

  (*ctl)->request_stop();
  runtime.stop_and_join();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

namespace {

int connect_metrics(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string http_get(int fd) {
  const char req[] = "GET /metrics HTTP/1.0\r\n\r\n";
  (void)!::write(fd, req, sizeof req - 1);
  std::string got;
  char buf[512];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) got.append(buf, static_cast<size_t>(n));
  return got;
}

}  // namespace

TEST(Ctl, MetricsBindLoopbackAndCidrAcl) {
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();

  // Empty allowlist: loopback bind serves the response.
  auto open_ep = server::MetricsHttp::create(0, "127.0.0.1", {});
  ASSERT_TRUE(open_ep.has_value());
  rt::spawn((*open_ep)->run(), runtime.reactor(0));
  int fd = connect_metrics((*open_ep)->port());
  ASSERT_TRUE(fd >= 0);
  auto body = http_get(fd);
  ::close(fd);
  EXPECT_TRUE(body.starts_with("HTTP/1.0 200 OK"));

  // Allowlist that excludes loopback: connection is dropped without a response.
  auto deny_cidr = core::Cidr::parse("10.0.0.0/8");
  ASSERT_TRUE(deny_cidr.has_value());
  std::vector<core::Cidr> allow{*deny_cidr};
  auto denied_ep = server::MetricsHttp::create(0, "127.0.0.1", std::move(allow));
  ASSERT_TRUE(denied_ep.has_value());
  rt::spawn((*denied_ep)->run(), runtime.reactor(0));
  fd = connect_metrics((*denied_ep)->port());
  ASSERT_TRUE(fd >= 0);
  EXPECT_STREQ(http_get(fd), "");
  ::close(fd);

  (*open_ep)->request_stop();
  (*denied_ep)->request_stop();
  runtime.stop_and_join();
}

TEST(Ctl, MetricsAndIdentityConfigKeys) {
  auto parsed = core::parse_config(
      "[server]\n"
      "metrics_bind = \"::1\"\n"
      "metrics_allow = [\"127.0.0.0/8\", \"::1/128\"]\n"
      "server_owner = \"nodeA\"\n"
      "server_scope = \"clusterX\"\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_STREQ(parsed->server.metrics_bind, "::1");
  EXPECT_EQ(parsed->server.metrics_allow.size(), 2u);
  EXPECT_STREQ(parsed->server.server_owner, "nodeA");
  EXPECT_STREQ(parsed->server.server_scope, "clusterX");
}

// Observability knobs (plan doc 10 §3.6/§3.7): slow-request threshold and error-ring
// size parse, defaults hold, and invalid values are rejected at load time.
TEST(Ctl, ObservabilityConfigKeys) {
  auto defaults = core::parse_config("[server]\n");
  ASSERT_TRUE(defaults.has_value());
  EXPECT_EQ(defaults->server.slow_request_ms, 1000u);
  EXPECT_EQ(defaults->server.error_ring, 64u);

  auto parsed = core::parse_config(
      "[server]\n"
      "slow_request_ms = 0\n"
      "error_ring = 512\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->server.slow_request_ms, 0u);
  EXPECT_EQ(parsed->server.error_ring, 512u);

  EXPECT_FALSE(core::parse_config("[server]\nerror_ring = 0\n").has_value());
  EXPECT_FALSE(core::parse_config("[server]\nslow_request_ms = 9999999999\n").has_value());
}

// ---- plan doc 10 §4: ops config keys, hot reload, ctl command surface --------------

TEST(Ctl, OpsConfigKeys) {
  auto parsed = core::parse_config(
      "[server]\n"
      "bind = \"127.0.0.1\"\n"
      "log_file = \"/var/log/lightnfs.log\"\n"
      "log_rotate_size = \"10MiB\"\n"
      "log_rotate_keep = 3\n"
      "[protocol]\n"
      "lease = \"90s\"\n"
      "grace = \"30s\"\n"
      "[limits]\n"
      "client_read_bps = \"10MiB\"\n"
      "client_write_bps = \"5MiB\"\n"
      "client_iops = 500\n"
      "[[export]]\n"
      "path = \"/tmp\"\n"
      "fsid = 1\n"
      "read_bps = \"50MiB\"\n"
      "write_bps = \"20MiB\"\n"
      "iops = 2000\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_STREQ(parsed->server.bind, "127.0.0.1");
  EXPECT_STREQ(parsed->server.log_file, "/var/log/lightnfs.log");
  EXPECT_EQ(parsed->server.log_rotate_size, 10u << 20);
  EXPECT_EQ(parsed->server.log_rotate_keep, 3u);
  EXPECT_EQ(parsed->server.lease_seconds, 90u);
  EXPECT_EQ(parsed->server.grace_seconds, 30u);  // decoupled from the lease
  EXPECT_EQ(parsed->server.client_read_bps, 10u << 20);
  EXPECT_EQ(parsed->server.client_write_bps, 5u << 20);
  EXPECT_EQ(parsed->server.client_iops, 500u);
  EXPECT_EQ(parsed->exports[0].read_bps, 50u << 20);
  EXPECT_EQ(parsed->exports[0].write_bps, 20u << 20);
  EXPECT_EQ(parsed->exports[0].iops, 2000u);

  // grace = "auto" keeps the lease-coupled default.
  auto auto_grace = core::parse_config("[protocol]\ngrace = \"auto\"\n");
  ASSERT_TRUE(auto_grace.has_value());
  EXPECT_EQ(auto_grace->server.grace_seconds, 0u);

  // A listener bind must be an address literal (validated with a full config).
  std::string valid =
      "[[export]]\npath = \"/tmp\"\nfsid = 1\nclients = [\"127.0.0.0/8\"]\n";
  auto good = core::parse_config("[server]\nbind = \"::1\"\n" + valid);
  ASSERT_TRUE(good.has_value());
  EXPECT_TRUE(core::validate_config(*good).has_value());
  auto bad = core::parse_config("[server]\nbind = \"nfs.example.com\"\n" + valid);
  ASSERT_TRUE(bad.has_value());
  EXPECT_FALSE(core::validate_config(*bad).has_value());
}

TEST(Ctl, ExportReloadDynamic) {
  core::ExportTable table;
  core::ExportConfig cfg;
  cfg.path = "/exp";
  cfg.fsid = 1;
  cfg.clients = {"127.0.0.0/8"};
  ASSERT_TRUE(table.add(cfg, std::make_unique<backend::MemoryBackend>(1)).has_value());

  sockaddr_storage peer{};
  auto* v4 = reinterpret_cast<sockaddr_in*>(&peer);
  v4->sin_family = AF_INET;
  inet_pton(AF_INET, "10.1.2.3", &v4->sin_addr);
  const auto* entry = table.by_fsid(1);
  EXPECT_FALSE(table.check_client(peer, *entry));

  core::Config fresh;
  core::ExportConfig updated = cfg;
  updated.clients = {"10.0.0.0/8"};
  updated.read_bps = 1u << 20;
  updated.iops = 100;
  fresh.exports.push_back(updated);
  auto report = table.reload_dynamic(fresh);
  EXPECT_TRUE(report.find("clients (1) and qos applied") != std::string::npos);
  EXPECT_TRUE(table.check_client(peer, *entry));  // allowlist swapped in place
  EXPECT_EQ(table.by_fsid(1)->qos.read_bytes.rate(), 1u << 20);
  EXPECT_EQ(table.by_fsid(1)->qos.ops.rate(), 100u);

  // Topology changes are reported, never applied.
  core::Config other;
  core::ExportConfig moved = updated;
  moved.path = "/elsewhere";
  other.exports.push_back(moved);
  core::ExportConfig added = updated;
  added.fsid = 2;
  other.exports.push_back(added);
  auto report2 = table.reload_dynamic(other);
  EXPECT_TRUE(report2.find("restart required") != std::string::npos);
  EXPECT_TRUE(report2.find("fsid=2") != std::string::npos);
  core::Config empty;
  auto report3 = table.reload_dynamic(empty);
  EXPECT_TRUE(report3.find("removed from config") != std::string::npos);
}

TEST(Ctl, AnswerCommandSurface) {
  server::CtlDeps deps{};  // no state/drc/hooks: commands degrade with a message
  EXPECT_TRUE(server::CtlServer::answer(deps, "version").starts_with("lightnfs "));
  EXPECT_STREQ(server::CtlServer::answer(deps, "ping --json"), "{\"ok\":true}\n");
  EXPECT_TRUE(server::CtlServer::answer(deps, "version --json").starts_with("{\"version\""));
  auto status = server::CtlServer::answer(deps, "status --json");
  EXPECT_TRUE(status.find("\"connections\":") != std::string::npos);
  EXPECT_TRUE(status.find("\"draining\":false") != std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "loglevel bogus").find("expected") !=
              std::string::npos);
  auto lv = server::CtlServer::answer(deps, "loglevel warn");
  EXPECT_TRUE(lv.find("warn") != std::string::npos);
  EXPECT_FALSE(log_enabled(LogLevel::kInfo));
  set_log_level(LogLevel::kInfo);  // restore for later tests
  EXPECT_TRUE(server::CtlServer::answer(deps, "reload").find("unavailable") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "drain --json").find("unavailable") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "grace-end").find("v4 disabled") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "kill-conn nope").find("numeric id") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "definitely-bogus").find("unknown") !=
              std::string::npos);
  // Hooks wired: reload/drain answers carry the hook's report.
  server::CtlDeps hooked{};
  hooked.reload = [] { return std::string("export fsid=1: clients (2) and qos applied\n"); };
  hooked.drain = [] { return std::string("draining: no new connections will be accepted\n"); };
  EXPECT_TRUE(server::CtlServer::answer(hooked, "reload").find("applied") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(hooked, "reload --json").find("\"reloaded\":true") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(hooked, "drain").find("draining") !=
              std::string::npos);
}

TEST(Ctl, ConnRegistryListAndKill) {
  int sv[2];
  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  transport::Peer peer{};
  uint64_t id = transport::ConnRegistry::instance().add(sv[0], peer);
  bool found = false;
  for (const auto& c : transport::ConnRegistry::instance().list())
    if (c.id == id) found = true;
  EXPECT_TRUE(found);
  EXPECT_TRUE(transport::ConnRegistry::instance().kill(id));
  char b;
  EXPECT_EQ(read(sv[1], &b, 1), 0);  // SHUT_RDWR: peer sees EOF
  transport::ConnRegistry::instance().remove(id);
  EXPECT_FALSE(transport::ConnRegistry::instance().kill(id));  // id gone
  close(sv[0]);
  close(sv[1]);
}
