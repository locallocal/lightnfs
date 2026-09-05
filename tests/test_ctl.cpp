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

#include "backend/local/local.hpp"
#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "mem_cluster_store.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "runtime/runtime.hpp"
#include "server/cluster_controller.hpp"
#include "server/ctl.hpp"
#include "state/state_mgr.hpp"
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

  // Delegation kill switch (plan doc 10 §5.2), default on.
  EXPECT_TRUE(auto_grace->server.delegations);
  auto no_deleg = core::parse_config("[protocol]\ndelegations = false\n");
  ASSERT_TRUE(no_deleg.has_value());
  EXPECT_FALSE(no_deleg->server.delegations);

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

// Multi-gateway failover (design 09 §9.3, plan 10 A1): the [cluster] section parses,
// its defaults hold, validation enforces the identity/role/path rules only while
// enabled, and bad values are rejected at parse time regardless.
TEST(Ctl, ClusterConfigKeys) {
  const std::string valid_export =
      "[[export]]\npath = \"/tmp\"\nfsid = 1\nclients = [\"127.0.0.0/8\"]\n";

  auto defaults = core::parse_config("[server]\n" + valid_export);
  ASSERT_TRUE(defaults.has_value());
  EXPECT_FALSE(defaults->cluster.enabled);
  EXPECT_STREQ(defaults->cluster.role, "auto");
  EXPECT_STREQ(defaults->cluster.takeover, "auto");
  EXPECT_EQ(defaults->cluster.fence_lease_ms, 3000u);
  EXPECT_FALSE(defaults->cluster.unsafe_skip_backend_checks);
  EXPECT_FALSE(core::cluster_node_name(defaults->cluster).empty());  // hostname default
  EXPECT_TRUE(core::validate_config(*defaults).has_value());

  const std::string cluster =
      "[cluster]\n"
      "enabled = true\n"
      "id = \"3f9c1e2a-6b7d-4c5e-9f10-2a3b4c5d6e7f\"\n"
      "shared_dir = \"/mnt/cephfs/.lightnfs-cluster/\"\n"
      "node = \"gw1\"\n"
      "role = \"standby\"\n"
      "fence_lease = \"1500ms\"\n"
      "takeover = \"manual\"\n"
      "unsafe_skip_backend_checks = true\n";
  auto parsed = core::parse_config(cluster + valid_export);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->cluster.enabled);
  EXPECT_STREQ(parsed->cluster.id, "3f9c1e2a-6b7d-4c5e-9f10-2a3b4c5d6e7f");
  EXPECT_STREQ(parsed->cluster.shared_dir, "/mnt/cephfs/.lightnfs-cluster");  // normalized
  EXPECT_STREQ(parsed->cluster.node, "gw1");
  EXPECT_STREQ(core::cluster_node_name(parsed->cluster), "gw1");
  EXPECT_STREQ(parsed->cluster.role, "standby");
  EXPECT_EQ(parsed->cluster.fence_lease_ms, 1500u);
  EXPECT_STREQ(parsed->cluster.takeover, "manual");
  EXPECT_TRUE(parsed->cluster.unsafe_skip_backend_checks);
  EXPECT_TRUE(core::validate_config(*parsed).has_value());
  // The reload path compares the whole section: equal to itself, different after a change.
  EXPECT_TRUE(parsed->cluster == parsed->cluster);
  EXPECT_FALSE(parsed->cluster == defaults->cluster);

  // Duration forms: seconds suffix, bare seconds; out-of-range and bad suffix rejected.
  auto secs = core::parse_config("[cluster]\nfence_lease = \"3s\"\n");
  ASSERT_TRUE(secs.has_value());
  EXPECT_EQ(secs->cluster.fence_lease_ms, 3000u);
  auto bare = core::parse_config("[cluster]\nfence_lease = \"2\"\n");
  ASSERT_TRUE(bare.has_value());
  EXPECT_EQ(bare->cluster.fence_lease_ms, 2000u);
  EXPECT_FALSE(core::parse_config("[cluster]\nfence_lease = \"100ms\"\n").has_value());
  EXPECT_FALSE(core::parse_config("[cluster]\nfence_lease = \"61s\"\n").has_value());
  EXPECT_FALSE(core::parse_config("[cluster]\nfence_lease = \"3m\"\n").has_value());
  // Type errors and unknown keys fail at parse time even when the section is disabled.
  EXPECT_FALSE(core::parse_config("[cluster]\nenabled = 1\n").has_value());
  EXPECT_FALSE(core::parse_config("[cluster]\nbogus = true\n").has_value());

  // Disabled: value-level rules are not applied, whatever the fields hold.
  auto disabled = core::parse_config(
      "[cluster]\nenabled = false\nrole = \"weird\"\nshared_dir = \"relative\"\n" +
      valid_export);
  ASSERT_TRUE(disabled.has_value());
  EXPECT_TRUE(core::validate_config(*disabled).has_value());

  // Enabled: each rule rejects on its own.
  auto rejects = [&](const std::string& section) {
    auto cfg = core::parse_config(section + valid_export);
    return cfg.has_value() && !core::validate_config(*cfg).has_value();
  };
  const std::string base =
      "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n";
  EXPECT_FALSE(rejects(base));
  // no id / no shared_dir
  EXPECT_TRUE(rejects("[cluster]\nenabled = true\nshared_dir = \"/srv/shared\"\n"));
  EXPECT_TRUE(rejects("[cluster]\nenabled = true\nid = \"short\"\nshared_dir = \"/x\"\n"));
  EXPECT_TRUE(rejects("[cluster]\nenabled = true\nid = \"bad id/with junk\"\n"
                      "shared_dir = \"/x\"\n"));
  EXPECT_TRUE(rejects("[cluster]\nenabled = true\nid = \"cluster-01\"\n"));
  EXPECT_TRUE(rejects("[cluster]\nenabled = true\nid = \"cluster-01\"\n"
                      "shared_dir = \"relative/dir\"\n"));
  EXPECT_TRUE(rejects(base + "role = \"primary\"\n"));
  EXPECT_TRUE(rejects(base + "takeover = \"never\"\n"));
  EXPECT_TRUE(rejects(base + "node = \"a/b\"\n"));
  EXPECT_TRUE(rejects(base + "takeover_hook = \"/nonexistent/hook.sh\"\n"));
  EXPECT_TRUE(rejects("[server]\nserver_owner = \"nodeA\"\n" + base));
  EXPECT_TRUE(rejects("[server]\nserver_scope = \"scopeA\"\n" + base));

  // takeover_hook must be an executable regular file.
  char tmpl[] = "/tmp/lnfs-hook-XXXXXX";
  int fd = ::mkstemp(tmpl);
  ASSERT_TRUE(fd >= 0);
  ::close(fd);
  EXPECT_TRUE(rejects(base + "takeover_hook = \"" + tmpl + "\"\n"));  // not executable
  ::chmod(tmpl, 0700);
  EXPECT_FALSE(rejects(base + "takeover_hook = \"" + tmpl + "\"\n"));
  ::unlink(tmpl);
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
  // No data plane attached (plan 10 A4: a standby gateway, or the window before the
  // listeners are up): the process-level commands work, everything that addresses
  // the exports / DRC / state / drain answers "not active".
  server::CtlDeps deps{};
  EXPECT_TRUE(server::CtlServer::answer(deps, "version").starts_with("lightnfs "));
  EXPECT_STREQ(server::CtlServer::answer(deps, "ping --json"), "{\"ok\":true}\n");
  EXPECT_TRUE(server::CtlServer::answer(deps, "version --json").starts_with("{\"version\""));
  auto status = server::CtlServer::answer(deps, "status --json");
  EXPECT_TRUE(status.find("\"connections\":") != std::string::npos);
  EXPECT_TRUE(status.find("\"draining\":false") != std::string::npos);
  EXPECT_TRUE(status.find("\"role\":\"standby\"") != std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "status").find("role=standby") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "loglevel bogus").find("expected") !=
              std::string::npos);
  auto lv = server::CtlServer::answer(deps, "loglevel warn");
  EXPECT_TRUE(lv.find("warn") != std::string::npos);
  EXPECT_FALSE(log_enabled(LogLevel::kInfo));
  set_log_level(LogLevel::kInfo);  // restore for later tests
  EXPECT_TRUE(server::CtlServer::answer(deps, "reload").find("unavailable") !=
              std::string::npos);
  EXPECT_STREQ(server::CtlServer::answer(deps, "drain"), "not active\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "drain --json"), "{\"error\":\"not active\"}\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "grace-end"), "not active\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "drc"), "not active\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "fdcache"), "not active\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "fdcache flush"), "not active\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "clear-poison --json"),
               "{\"error\":\"not active\"}\n");
  EXPECT_TRUE(server::CtlServer::answer(deps, "kill-conn nope").find("numeric id") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "definitely-bogus").find("unknown") !=
              std::string::npos);
  // No cluster controller (single gateway): every `cluster *` answers not enabled.
  EXPECT_STREQ(server::CtlServer::answer(deps, "cluster status"), "cluster: not enabled\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "cluster takeover --force --json"),
               "{\"error\":\"not enabled\"}\n");
  EXPECT_STREQ(server::CtlServer::answer(deps, "cluster standby"), "cluster: not enabled\n");

  // An attached but empty plane: the per-feature messages, and role=active.
  server::DataPlane empty{};
  auto attached = server::CtlDeps::with_plane(&empty);
  EXPECT_TRUE(server::CtlServer::answer(attached, "status").find("role=active") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(attached, "drain --json").find("unavailable") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(attached, "grace-end").find("v4 disabled") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(attached, "drc").find("drc disabled") !=
              std::string::npos);
  // A role hook (the cluster controller's) overrides the derived text.
  attached.role = [] { return std::string("draining"); };
  EXPECT_TRUE(server::CtlServer::answer(attached, "status --json")
                  .find("\"role\":\"draining\"") != std::string::npos);

  // Attach / detach through the shared slot switches the answers live.
  auto slot = std::make_shared<server::DataPlaneSlot>(nullptr);
  server::CtlDeps switchable{};
  switchable.plane = slot;
  EXPECT_STREQ(server::CtlServer::answer(switchable, "drc"), "not active\n");
  slot->store(&empty);
  EXPECT_TRUE(server::CtlServer::answer(switchable, "drc").find("drc disabled") !=
              std::string::npos);
  slot->store(nullptr);
  EXPECT_STREQ(server::CtlServer::answer(switchable, "drc"), "not active\n");

  // Hooks wired: reload (process-level) and drain (data plane) carry their reports.
  server::DataPlane plane{};
  plane.drain = [] { return std::string("draining: no new connections will be accepted\n"); };
  auto hooked = server::CtlDeps::with_plane(&plane);
  hooked.reload = [] { return std::string("export fsid=1: clients (2) and qos applied\n"); };
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

// ---- plan doc 10 §7.1: the remaining ctl command surface + metrics HTTP contract ----

namespace {

std::string joined_calls(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : " ") + s;
  return out;
}

template <class T>
T run_task(rt::Runtime& runtime, rt::Task<T> task) {
  std::mutex mu;
  std::condition_variable cv;
  std::optional<T> result;
  rt::spawn([](rt::Task<T> work, std::mutex* mu, std::condition_variable* cv,
               std::optional<T>* out) -> rt::Task<void> {
    auto value = co_await std::move(work);
    {
      std::lock_guard lock(*mu);
      out->emplace(std::move(value));
      cv->notify_one();
    }
  }(std::move(task), &mu, &cv, &result),
            runtime.reactor(0));
  std::unique_lock lock(mu);
  cv.wait(lock, [&] { return result.has_value(); });
  return std::move(*result);
}

}  // namespace

TEST(Ctl, MetricsDumpErrorsAndConnsCommands) {
  server::CtlDeps deps{};
  // `metrics` answers the same exposition the HTTP endpoint serves.
  auto metrics = server::CtlServer::answer(deps, "metrics");
  EXPECT_TRUE(metrics.find("lightnfs_rpc_garbage_total") != std::string::npos);
  // dump-errors in both formats.
  EXPECT_TRUE(server::CtlServer::answer(deps, "dump-errors").find("total_errors=") !=
              std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, "dump-errors --json")
                  .find("\"total_errors\":") != std::string::npos);

  // conns rendering + kill-conn success/not-found through the command surface.
  int sv[2];
  ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  transport::Peer peer{};
  uint64_t id = transport::ConnRegistry::instance().add(sv[0], peer);
  auto conns = server::CtlServer::answer(deps, "conns");
  EXPECT_TRUE(conns.find(std::format("id={}", id)) != std::string::npos);
  auto conns_json = server::CtlServer::answer(deps, "conns --json");
  EXPECT_TRUE(conns_json.find(std::format("\"id\":{}", id)) != std::string::npos);
  auto killed = server::CtlServer::answer(deps, std::format("kill-conn {}", id));
  EXPECT_TRUE(killed.find("shut down") != std::string::npos);
  char b;
  EXPECT_EQ(read(sv[1], &b, 1), 0);  // the shutdown really reached the socket
  transport::ConnRegistry::instance().remove(id);
  EXPECT_TRUE(server::CtlServer::answer(deps, std::format("kill-conn {}", id))
                  .find("not found") != std::string::npos);
  EXPECT_TRUE(server::CtlServer::answer(deps, std::format("kill-conn {} --json", id))
                  .find("\"killed\":false") != std::string::npos);
  close(sv[0]);
  close(sv[1]);
}

// plan 10 C3: `cluster status | takeover [--force] | standby` against a real controller
// over the in-memory store (manual takeover policy, inline hooks that record their order).
TEST(Ctl, ClusterCommands) {
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  {
    test::MemClusterStore store;
    store.digests["gw2"] = "sha256:b";
    store.digests["gw1"] = "sha256:a";
    std::vector<std::string> calls;
    server::ClusterController::Hooks hooks;
    hooks.activate = [&](uint64_t) -> Result<void> {
      calls.push_back("activate");
      return {};
    };
    hooks.deactivate = [&] { calls.push_back("deactivate"); };
    hooks.backend_reset = [&] { calls.push_back("reset"); };
    core::ClusterConfig cfg;
    cfg.enabled = true;
    cfg.id = "cluster-ctl-test";
    cfg.shared_dir = "/mnt/shared/.lightnfs-cluster";
    cfg.node = "gw1";
    cfg.takeover = "manual";
    cfg.fence_lease_ms = 1000;
    server::ClusterController cc(cfg, store, std::move(hooks));
    server::CtlDeps deps{};
    deps.cluster = &cc;
    auto ask = [&](const char* line) {
      return run_task(runtime, server::CtlServer::answer_async(deps, line));
    };

    // Standby, no fence seen yet: the documented text line, then the JSON twin.
    EXPECT_STREQ(ask("cluster status"),
                 "role=standby node=gw1 epoch=0 fence_owner=none fence_epoch=- "
                 "fence_age_ms=- fence_expires_in_ms=- "
                 "shared_dir=/mnt/shared/.lightnfs-cluster peers=gw1,gw2 takeover=manual "
                 "takeovers=0 fence_lost=0 activation_failures=0 last_activation_ms=0\n");
    auto js = ask("cluster status --json");
    EXPECT_TRUE(js.find("\"role\":\"standby\",\"node\":\"gw1\",\"epoch\":0,"
                        "\"fence_owner\":null,\"fence_epoch\":null,\"fence_age_ms\":null,"
                        "\"fence_expires_in_ms\":null,"
                        "\"shared_dir\":\"/mnt/shared/.lightnfs-cluster\","
                        "\"peers\":[\"gw1\",\"gw2\"],\"takeover\":\"manual\"") !=
                std::string::npos);
    // The process-level `status` derives its role from the controller too.
    EXPECT_TRUE(server::CtlServer::answer(deps, "status").find("role=standby") !=
                std::string::npos);
    EXPECT_TRUE(ask("cluster standby").find("not active (role=standby)") != std::string::npos);
    EXPECT_STREQ(ask("cluster bogus"), "cluster: expected status|takeover [--force]|standby\n");
    EXPECT_STREQ(ask("cluster --json"), "{\"error\":\"bad subcommand\"}\n");

    // A live fence held by gw2 (seen by one Standby tick): plain takeover is refused
    // and names the holder; --force wins, the hooks run in order, status flips.
    store.taken_by("gw2", 7);
    cc.tick();
    EXPECT_TRUE(ask("cluster status").find("fence_owner=gw2 fence_epoch=7 fence_age_ms=") !=
                std::string::npos);
    auto refused = ask("cluster takeover");
    EXPECT_STREQ(refused, "cluster: fence held by gw2 (retry with --force to take it)\n");
    EXPECT_STREQ(ask("cluster takeover --json"),
                 "{\"error\":\"fence held by gw2 (retry with --force to take it)\"}\n");
    EXPECT_TRUE(cc.role() == server::Role::kStandby);
    EXPECT_STREQ(ask("cluster takeover --force"),
                 "takeover started: node=gw1 epoch=1 role=active (forced)\n");
    EXPECT_STREQ(joined_calls(calls), "activate");
    EXPECT_TRUE(cc.role() == server::Role::kActive);
    auto active = ask("cluster status");
    EXPECT_TRUE(active.find("role=active node=gw1 epoch=1 fence_owner=gw1 fence_epoch=1 "
                            "fence_age_ms=") != std::string::npos);
    EXPECT_TRUE(active.find("takeovers=1 fence_lost=0") != std::string::npos);
    EXPECT_TRUE(ask("cluster status --json").find("\"fence_owner\":\"gw1\"") !=
                std::string::npos);
    EXPECT_TRUE(server::CtlServer::answer(deps, "status --json")
                    .find("\"role\":\"active\"") != std::string::npos);
    // Already active: takeover is refused with the role.
    EXPECT_STREQ(ask("cluster takeover"), "cluster: not standby (role=active)\n");
    EXPECT_STREQ(ask("cluster takeover --json"),
                 "{\"error\":\"not standby (role=active)\"}\n");

    // standby: drains (deactivate → backend reset), releases our fence, back to standby.
    EXPECT_STREQ(ask("cluster standby --json"), "{\"standby\":true}\n");
    EXPECT_STREQ(joined_calls(calls), "activate deactivate reset");
    EXPECT_TRUE(cc.role() == server::Role::kStandby);
    EXPECT_FALSE(store.fence.has_value());
    EXPECT_TRUE(ask("cluster status").find("role=standby node=gw1 epoch=0 fence_owner=gw1") !=
                std::string::npos);  // the last record seen was ours
    // An expired fence needs no force; JSON success carries the new epoch.
    store.taken_by("gw2", 9);
    store.age_out();
    EXPECT_STREQ(ask("cluster takeover --json"),
                 "{\"takeover\":true,\"forced\":false,\"epoch\":2,\"role\":\"active\"}\n");
    EXPECT_STREQ(ask("cluster standby"), "standby requested: draining\n");
    // A store that cannot list peers still answers, with the peers unknown.
    store.fail_list = errno_from(EIO);
    EXPECT_TRUE(ask("cluster status").find(" peers=? ") != std::string::npos);
    EXPECT_TRUE(ask("cluster status --json").find("\"peers\":null") != std::string::npos);
  }
  runtime.stop_and_join();
}

TEST(Ctl, FdcacheAndClearPoisonCommands) {
  char tmpl[] = "/tmp/lnfs-ctlfd-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  rt::Runtime runtime({.reactors = 1, .offload_threads = 2});
  runtime.start();
  {
    // A table with only a memory export: both commands answer "no local/gluster/cephfs exports".
    core::ExportTable mem_only;
    core::ExportConfig mcfg;
    mcfg.path = "/export/mem";
    mcfg.fsid = 71;
    mcfg.clients = {"127.0.0.0/8"};
    ASSERT_TRUE(mem_only.add(mcfg, std::make_unique<backend::MemoryBackend>(71)).has_value());
    server::DataPlane mem_plane{.exports = &mem_only};
    auto mem_deps = server::CtlDeps::with_plane(&mem_plane);
    EXPECT_STREQ(server::CtlServer::answer(mem_deps, "fdcache"), "no local/gluster/cephfs exports\n");
    EXPECT_STREQ(server::CtlServer::answer(mem_deps, "clear-poison"), "no local/gluster/cephfs exports\n");

    // A local export: stats render per export, flush and clear-poison count real work.
    auto made = backend::LocalBackend::create({.path = dir, .fsid = 72});
    ASSERT_TRUE(made.has_value());
    auto* local = made->get();
    core::ExportTable exports;
    core::ExportConfig cfg;
    cfg.path = "/export/data";
    cfg.fsid = 72;
    cfg.clients = {"127.0.0.0/8"};
    ASSERT_TRUE(exports.add(cfg, std::move(*made)).has_value());
    server::DataPlane plane{.exports = &exports};
    auto deps = server::CtlDeps::with_plane(&plane);

    auto stats = server::CtlServer::answer(deps, "fdcache");
    EXPECT_TRUE(stats.find("export=/export/data") != std::string::npos);
    EXPECT_TRUE(server::CtlServer::answer(deps, "fdcache --json")
                    .find("\"export\":\"/export/data\"") != std::string::npos);
    EXPECT_TRUE(server::CtlServer::answer(deps, "fdcache flush").find("flushed") !=
                std::string::npos);

    auto root = run_task(runtime, local->root());
    ASSERT_TRUE(root.has_value());
    local->poison((*root)->id());
    EXPECT_STREQ(server::CtlServer::answer(deps, "clear-poison"),
                 "cleared 1 poison marks\n");
    EXPECT_STREQ(server::CtlServer::answer(deps, "clear-poison --json"),
                 "{\"cleared\":0}\n");
  }
  runtime.stop_and_join();
  std::filesystem::remove_all(dir);
}

TEST(Ctl, AnswerAsyncStateExpireAndDrcFlush) {
  char tmpl[] = "/tmp/lnfs-ctlstate-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  {
    state::StateMgr mgr({.boot_epoch = 9, .state_dir = dir});
    rpc::Drc drc({});
    server::DataPlane plane{.drc = &drc, .state = &mgr};
    auto deps = server::CtlDeps::with_plane(&plane);

    auto state = run_task(runtime, server::CtlServer::answer_async(deps, "state"));
    EXPECT_TRUE(state.find("clients=0") != std::string::npos);
    EXPECT_TRUE(state.find("lock_owners=0") != std::string::npos);
    auto state_json =
        run_task(runtime, server::CtlServer::answer_async(deps, "state --json"));
    EXPECT_TRUE(state_json.find("\"clients\":0") != std::string::npos);

    EXPECT_TRUE(run_task(runtime,
                         server::CtlServer::answer_async(deps, "expire-client zz"))
                    .find("bad clientid") != std::string::npos);
    EXPECT_TRUE(run_task(runtime,
                         server::CtlServer::answer_async(deps, "expire-client 0x99"))
                    .find("nfs4 status") != std::string::npos);

    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(deps, "drc flush")),
                 "flushed 0 drc entries\n");
    EXPECT_TRUE(run_task(runtime,
                         server::CtlServer::answer_async(deps, "drc flush --json"))
                    .find("\"flushed\":0") != std::string::npos);

    // An attached plane with null members degrades like the sync surface; no plane
    // at all is "not active" (plan 10 A4).
    server::DataPlane empty{};
    auto none = server::CtlDeps::with_plane(&empty);
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(none, "state")),
                 "v4 disabled\n");
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(none, "drc flush")),
                 "drc disabled\n");
    server::CtlDeps detached{};
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(detached, "state")),
                 "not active\n");
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(detached, "expire-client 1")),
                 "not active\n");
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(detached, "drc flush --json")),
                 "{\"error\":\"not active\"}\n");
    // Unknown async commands fall through to the sync answer.
    EXPECT_STREQ(run_task(runtime, server::CtlServer::answer_async(none, "ping")),
                 "pong\n");
  }
  runtime.stop_and_join();
  std::filesystem::remove_all(dir);
}

TEST(Ctl, MetricsHttpHeadersAndBodyContract) {
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  auto ep = server::MetricsHttp::create(0, "127.0.0.1", {});
  ASSERT_TRUE(ep.has_value());
  rt::spawn((*ep)->run(), runtime.reactor(0));
  int fd = connect_metrics((*ep)->port());
  ASSERT_TRUE(fd >= 0);
  auto response = http_get(fd);
  close(fd);
  ASSERT_TRUE(response.starts_with("HTTP/1.0 200 OK\r\n"));
  EXPECT_TRUE(response.find("Content-Type: text/plain; version=0.0.4\r\n") !=
              std::string::npos);
  auto split = response.find("\r\n\r\n");
  ASSERT_TRUE(split != std::string::npos);
  std::string body = response.substr(split + 4);
  // Content-Length matches the actual body, and the body is the Prometheus exposition.
  auto cl_pos = response.find("Content-Length: ");
  ASSERT_TRUE(cl_pos != std::string::npos);
  EXPECT_EQ(static_cast<size_t>(std::stoull(response.substr(cl_pos + 16))), body.size());
  EXPECT_TRUE(body.starts_with("# TYPE lightnfs_v3_calls_total counter"));
  EXPECT_TRUE(body.find("lightnfs_rpc_garbage_total") != std::string::npos);
  // Invalid bind address is rejected up front, not at first use.
  EXPECT_FALSE(server::MetricsHttp::create(0, "not-an-ip", {}).has_value());
  (*ep)->request_stop();
  runtime.stop_and_join();
}
