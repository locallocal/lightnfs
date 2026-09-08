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
#include "core/fs_owner_view.hpp"
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

TEST(Ctl, ActiveActiveConfigKeys) {
  // Plan 12 A1: `[cluster] mode` / `node_address` and `[[export]] nodes`.
  const std::string exp_a = "[[export]]\npath = \"/tmp\"\nfsid = 1\nclients = [\"127.0.0.0/8\"]\n";
  const std::string exp_b = "[[export]]\npath = \"/var\"\nfsid = 2\nclients = [\"127.0.0.0/8\"]\n";
  const std::string base =
      "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n";
  auto parse_ok = [](const std::string& text) {
    auto cfg = core::parse_config(text);
    return cfg.has_value() && core::validate_config(*cfg).has_value();
  };
  auto rejects = [](const std::string& text) {
    auto cfg = core::parse_config(text);
    return cfg.has_value() && !core::validate_config(*cfg).has_value();
  };

  // Defaults: failover, no address, no owner lists; single-gateway configs untouched.
  auto defaults = core::parse_config("[server]\n" + exp_a);
  ASSERT_TRUE(defaults.has_value());
  EXPECT_STREQ(defaults->cluster.mode, "failover");
  EXPECT_TRUE(defaults->cluster.node_address.empty());
  EXPECT_TRUE(defaults->exports[0].nodes.empty());
  EXPECT_FALSE(core::cluster_active_active(defaults->cluster));
  EXPECT_TRUE(core::validate_config(*defaults).has_value());

  // Full active-active example (design 10 §10.10).
  const std::string aa = base + "mode = \"active-active\"\nnode_address = \"10.0.0.11:2049\"\n";
  auto parsed = core::parse_config(aa + exp_a + "nodes = [\"gw1\", \"gw2\", \"gw3\"]\n" + exp_b +
                                   "nodes = [\"gw2\", \"gw3\", \"gw1\"]\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_STREQ(parsed->cluster.mode, "active-active");
  EXPECT_STREQ(parsed->cluster.node_address, "10.0.0.11:2049");
  EXPECT_TRUE(core::cluster_active_active(parsed->cluster));
  ASSERT_TRUE(parsed->exports.size() == 2u);
  ASSERT_TRUE(parsed->exports[0].nodes.size() == 3u);
  EXPECT_STREQ(parsed->exports[0].nodes[0], "gw1");
  EXPECT_STREQ(parsed->exports[1].nodes[0], "gw2");
  EXPECT_TRUE(core::validate_config(*parsed).has_value());
  // The whole section takes part in the reload comparison (restart-required report).
  auto other_addr = core::parse_config(base + "mode = \"active-active\"\n"
                                              "node_address = \"10.0.0.12:2049\"\n" + exp_a);
  ASSERT_TRUE(other_addr.has_value());
  EXPECT_FALSE(parsed->cluster == other_addr->cluster);

  // Type / value errors caught at parse time.
  EXPECT_FALSE(core::parse_config("[cluster]\nmode = 1\n").has_value());
  EXPECT_FALSE(core::parse_config("[[export]]\nnodes = \"gw1\"\n").has_value());

  // Under failover the new keys are warned about and ignored, never rejected.
  EXPECT_TRUE(parse_ok(base + "node_address = \"10.0.0.11:2049\"\n" + exp_a +
                       "nodes = [\"gw1\"]\n"));
  // Disabled: even a bad mode is ignored.
  EXPECT_TRUE(parse_ok("[cluster]\nenabled = false\nmode = \"weird\"\n" + exp_a));

  // Enabled: each active-active rule rejects on its own.
  const std::string a_nodes = exp_a + "nodes = [\"gw1\", \"gw2\"]\n";
  EXPECT_TRUE(parse_ok(aa + a_nodes));
  EXPECT_TRUE(rejects(base + "mode = \"multi\"\n" + a_nodes));               // unknown mode
  EXPECT_TRUE(rejects(base + "mode = \"active-active\"\n" + a_nodes));       // no node_address
  EXPECT_TRUE(rejects(aa + exp_a));                                          // nodes missing
  EXPECT_TRUE(rejects(aa + exp_a + "nodes = []\n"));                         // nodes empty
  EXPECT_TRUE(rejects(aa + exp_a + "nodes = [\"gw1\", \"gw1\"]\n"));         // duplicate
  EXPECT_TRUE(rejects(aa + exp_a + "nodes = [\"gw 1\"]\n"));                 // bad name
  EXPECT_TRUE(rejects(aa + exp_a + "nodes = [\"a/b\"]\n"));                  // bad name
  EXPECT_TRUE(rejects(aa + a_nodes + exp_b));                                // one export lacks nodes
  EXPECT_TRUE(rejects(aa + "role = \"active\"\n" + a_nodes));                // role must be auto
  EXPECT_TRUE(rejects(aa + "role = \"standby\"\n" + a_nodes));
  EXPECT_TRUE(parse_ok(aa + "role = \"auto\"\ntakeover = \"manual\"\n" + a_nodes));
  // node_address forms.
  auto with_addr = [&](const std::string& addr) {
    return base + "mode = \"active-active\"\nnode_address = \"" + addr + "\"\n" + a_nodes;
  };
  EXPECT_TRUE(parse_ok(with_addr("gw1.example.net:2049")));
  EXPECT_TRUE(parse_ok(with_addr("[fd00::11]:2049")));
  EXPECT_TRUE(rejects(with_addr("10.0.0.11")));         // no port
  EXPECT_TRUE(rejects(with_addr("10.0.0.11:")));        // empty port
  EXPECT_TRUE(rejects(with_addr(":2049")));             // empty host
  EXPECT_TRUE(rejects(with_addr("10.0.0.11:0")));       // port range
  EXPECT_TRUE(rejects(with_addr("10.0.0.11:70000")));
  EXPECT_TRUE(rejects(with_addr("10.0.0.11:20a9")));
  EXPECT_TRUE(rejects(with_addr("fd00::11:2049")));     // unbracketed v6
  EXPECT_TRUE(core::valid_node_address("[::1]:1") && !core::valid_node_address("[::1]:"));

  // Gluster / Lustre isolation (design 10 §10.6): exports on one volume / mount must
  // list the same nodes; different volumes may differ; CephFS is per fsid and free.
  auto gluster = [&](const std::string& vol, const std::string& fsid, const std::string& nodes) {
    return "[[export]]\npath = \"/vol" + fsid + "\"\nfsid = " + fsid +
           "\nbackend = \"gluster\"\nclients = [\"127.0.0.0/8\"]\nnodes = " + nodes +
           "\n[export.gluster]\nvolume = \"" + vol + "\"\nhost = \"h\"\n";
  };
  auto lustre = [&](const std::string& mnt, const std::string& fsid, const std::string& nodes) {
    // Lustre exports are real directories under the client mount: use ones that exist.
    return "[[export]]\npath = \"" + (fsid == "1" ? std::string("/tmp") : std::string("/var")) +
           "\"\nfsid = " + fsid +
           "\nbackend = \"lustre\"\nclients = [\"127.0.0.0/8\"]\nnodes = " + nodes +
           "\n[export.lustre]\nmount = \"" + mnt + "\"\n";
  };
  const std::string same = "[\"gw1\", \"gw2\"]", diff = "[\"gw2\", \"gw1\"]";
  auto gluster_ok = core::parse_config(aa + gluster("v1", "1", same) + gluster("v1", "2", same));
  ASSERT_TRUE(gluster_ok.has_value());
  auto gluster_bad = core::parse_config(aa + gluster("v1", "1", same) + gluster("v1", "2", diff));
  ASSERT_TRUE(gluster_bad.has_value());
  auto gluster_two = core::parse_config(aa + gluster("v1", "1", same) + gluster("v2", "2", diff));
  ASSERT_TRUE(gluster_two.has_value());
  auto lustre_bad = core::parse_config(aa + lustre("/mnt/l", "1", same) + lustre("/mnt/l", "2", diff));
  ASSERT_TRUE(lustre_bad.has_value());
  auto lustre_two = core::parse_config(aa + lustre("/mnt/l", "1", same) + lustre("/mnt/m", "2", diff));
  ASSERT_TRUE(lustre_two.has_value());
  if (backend::find_backend("gluster")) {  // built with the Gluster backend
    EXPECT_TRUE(core::validate_config(*gluster_ok).has_value());
    EXPECT_FALSE(core::validate_config(*gluster_bad).has_value());
    EXPECT_TRUE(core::validate_config(*gluster_two).has_value());
  }
  if (backend::find_backend("lustre")) {
    EXPECT_FALSE(core::validate_config(*lustre_bad).has_value());
    EXPECT_TRUE(core::validate_config(*lustre_two).has_value());
  }
  // The same lists under failover mode are not checked (ignored with a warning).
  auto gluster_bad_failover =
      core::parse_config(base + gluster("v1", "1", same) + gluster("v1", "2", diff));
  ASSERT_TRUE(gluster_bad_failover.has_value());
  if (backend::find_backend("gluster"))
    EXPECT_TRUE(core::validate_config(*gluster_bad_failover).has_value());

  // Export digest: the owner order is cluster identity; an absent list adds nothing,
  // so failover digests are unchanged.
  auto plain = core::parse_config(base + exp_a);
  ASSERT_TRUE(plain.has_value());
  EXPECT_STREQ(core::canonical_exports_digest(*plain), core::canonical_exports_digest(*defaults));
  auto n1 = core::parse_config(aa + exp_a + "nodes = [\"gw1\", \"gw2\"]\n");
  auto n2 = core::parse_config(aa + exp_a + "nodes = [\"gw2\", \"gw1\"]\n");
  ASSERT_TRUE(n1.has_value() && n2.has_value());
  EXPECT_TRUE(core::canonical_exports_digest(*n1) != core::canonical_exports_digest(*n2));
  EXPECT_TRUE(core::canonical_exports_digest(*n1) != core::canonical_exports_digest(*plain));
  EXPECT_TRUE(core::canonical_exports_text(*n1).find("  nodes=gw1,gw2\n") != std::string::npos);

  // ExportTable carries the list; a changed list is a restart-required reload item.
  core::ExportTable table;
  core::ExportConfig cfg;
  cfg.path = "/exp";
  cfg.fsid = 1;
  cfg.clients = {"127.0.0.0/8"};
  cfg.nodes = {"gw1", "gw2"};
  ASSERT_TRUE(table.add(cfg, std::make_unique<backend::MemoryBackend>(1)).has_value());
  ASSERT_TRUE(table.by_fsid(1)->nodes.size() == 2u);
  EXPECT_STREQ(table.by_fsid(1)->nodes[1], "gw2");
  core::Config fresh;
  fresh.exports.push_back(cfg);
  EXPECT_TRUE(table.reload_dynamic(fresh).find("nodes changed") == std::string::npos);
  fresh.exports[0].nodes = {"gw2", "gw1"};
  EXPECT_TRUE(table.reload_dynamic(fresh).find("export fsid=1: nodes changed, restart required") !=
              std::string::npos);
  EXPECT_STREQ(table.by_fsid(1)->nodes[0], "gw1");  // never applied live
}

TEST(Ctl, CatalogConfigKeys) {
  // Plan 12 A1: `[cluster] exports_source` / `catalog_refresh` and `[backend_defaults.*]`.
  const std::string exp_a = "[[export]]\npath = \"/tmp\"\nfsid = 1\nclients = [\"127.0.0.0/8\"]\n";
  const std::string cluster =
      "[cluster]\nenabled = true\nid = \"cluster-01\"\nshared_dir = \"/srv/shared\"\n";
  auto parse_ok = [](const std::string& text) {
    auto cfg = core::parse_config(text);
    return cfg.has_value() && core::validate_config(*cfg).has_value();
  };
  auto rejects = [](const std::string& text) {
    auto cfg = core::parse_config(text);
    return cfg.has_value() && !core::validate_config(*cfg).has_value();
  };

  // Defaults: local exports, auto refresh, no defaults table; today's configs untouched.
  auto defaults = core::parse_config("[server]\n" + exp_a);
  ASSERT_TRUE(defaults.has_value());
  EXPECT_STREQ(defaults->cluster.exports_source, "local");
  EXPECT_STREQ(defaults->cluster.catalog_refresh, "auto");
  EXPECT_TRUE(defaults->backend_defaults.empty());
  EXPECT_FALSE(core::cluster_catalog_exports(defaults->cluster));
  EXPECT_TRUE(core::validate_config(*defaults).has_value());
  EXPECT_TRUE(rejects("[server]\n"));  // local mode still insists on exports

  // Catalog mode: cluster on, no local exports, an empty table is fine before the
  // first publish; the per-node defaults table is parsed and kept.
  const std::string catalog = cluster +
                              "exports_source = \"catalog\"\ncatalog_refresh = \"manual\"\n"
                              "[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\n"
                              "keyring = \"/etc/ceph/gw1.keyring\"\nfd_cache = 4096\n"
                              "[backend_defaults.gluster]\nlog_file = \"/var/log/gfapi.log\"\n";
  auto cat = core::parse_config(catalog);
  ASSERT_TRUE(cat.has_value());
  EXPECT_TRUE(core::validate_config(*cat).has_value());
  EXPECT_TRUE(core::cluster_catalog_exports(cat->cluster));
  EXPECT_STREQ(cat->cluster.catalog_refresh, "manual");
  EXPECT_TRUE(cat->exports.empty());
  ASSERT_TRUE(cat->backend_defaults.size() == 2u);
  EXPECT_STREQ(cat->backend_defaults.at("cephfs").values.at("conf"), "/etc/ceph/ceph.conf");
  EXPECT_STREQ(cat->backend_defaults.at("cephfs").values.at("fd_cache"), "4096");
  EXPECT_STREQ(cat->backend_defaults.at("gluster").values.at("log_file"), "/var/log/gfapi.log");
  // Design 11 §11.2's local example validates as a whole.
  EXPECT_TRUE(parse_ok(cluster + "mode = \"active-active\"\nnode = \"gw1\"\n"
                                 "node_address = \"10.0.0.11:2049\"\n"
                                 "exports_source = \"catalog\"\ncatalog_refresh = \"auto\"\n"
                                 "[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\n"
                                 "name = \"client.gw1\"\n"));

  // Rejected: a local [[export]] beside the catalog (two sources of truth), catalog
  // without the cluster section, bad enum values.
  EXPECT_TRUE(rejects(cluster + "exports_source = \"catalog\"\n" + exp_a));
  EXPECT_TRUE(rejects("[cluster]\nexports_source = \"catalog\"\n"));
  EXPECT_TRUE(rejects(cluster + "exports_source = \"shared\"\n" + exp_a));
  EXPECT_TRUE(rejects(cluster + "catalog_refresh = \"bogus\"\n" + exp_a));
  // Rejected at parse time: a cluster-wide key in [backend_defaults], an empty backend
  // name, a defaults table with no section name at all.
  EXPECT_FALSE(
      core::parse_config(catalog + "[backend_defaults.cephfs]\nfs_name = \"x\"\n").has_value());
  EXPECT_FALSE(core::parse_config(catalog + "[backend_defaults.]\nconf = \"x\"\n").has_value());
  EXPECT_FALSE(core::parse_config("[backend_defaults]\nconf = \"x\"\n").has_value());
  EXPECT_TRUE(core::per_node_backend_key("keyring"));
  EXPECT_FALSE(core::per_node_backend_key("volume"));

  // Local mode with a defaults table: ignored with a warning, not an error.
  EXPECT_TRUE(parse_ok(cluster + exp_a + "[backend_defaults.cephfs]\nconf = \"/x\"\n"));
  auto local_with_defaults =
      core::parse_config("[server]\n" + exp_a + "[backend_defaults.local]\nfd_cache = 1\n");
  ASSERT_TRUE(local_with_defaults.has_value());
  EXPECT_FALSE(core::cluster_catalog_exports(local_with_defaults->cluster));
  EXPECT_TRUE(core::validate_config(*local_with_defaults).has_value());

  // The [backend_defaults] section does not leak into the export that precedes it, and
  // an [export.*] table after it still binds to the export (section switching).
  auto mixed =
      core::parse_config("[server]\n" + exp_a + "[backend_defaults.local]\nfd_cache = 1\n" +
                         "[[export]]\npath = \"/var\"\nfsid = 2\n[export.local]\nfd_cache = 2\n");
  ASSERT_TRUE(mixed.has_value());
  EXPECT_TRUE(mixed->exports[0].backend_config.values.empty());
  EXPECT_STREQ(mixed->exports[1].backend_config.values.at("fd_cache"), "2");
  // Neither key nor table enters the export digest (they are not export identity).
  auto plain = core::parse_config(cluster + exp_a);
  ASSERT_TRUE(plain.has_value());
  EXPECT_STREQ(core::canonical_exports_digest(*plain),
               core::canonical_exports_digest(
                   *core::parse_config(cluster + "catalog_refresh = \"manual\"\n" + exp_a +
                                       "[backend_defaults.local]\nfd_cache = 1\n")));
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
// Active-active (plan 12 C4): `cluster status` per export, `takeover <fsid>` and
// `standby <fsid>` on one export, the fsid-less forms refused, JSON twins throughout.
TEST(Ctl, ClusterFsCommands) {
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  {
    test::MemClusterStore store;
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.renew_fences("gw2", std::chrono::seconds(60));  // gw2 is alive
    core::ExportTable exports;
    for (uint32_t fsid = 1; fsid <= 3; ++fsid) {
      core::ExportConfig ec;
      ec.path = "/export/" + std::to_string(fsid);
      ec.fsid = fsid;
      ec.clients = {"127.0.0.0/8"};
      ec.nodes =
          fsid == 3 ? std::vector<std::string>{"gw2"} : std::vector<std::string>{"gw1", "gw2"};
      ASSERT_TRUE(exports.add(ec, std::make_unique<backend::MemoryBackend>(fsid)).has_value());
    }
    std::vector<std::string> calls;
    server::FsClusterController::Hooks hooks;
    hooks.activate_fs = [&](uint32_t fsid, uint64_t) -> Result<void> {
      calls.push_back("activate:" + std::to_string(fsid));
      return {};
    };
    hooks.deactivate_fs = [&](uint32_t fsid) {
      calls.push_back("deactivate:" + std::to_string(fsid));
    };
    core::ClusterConfig cfg;
    cfg.enabled = true;
    cfg.mode = "active-active";
    cfg.id = "cluster-ctl-test";
    cfg.shared_dir = "/mnt/shared/.lightnfs-cluster";
    cfg.node = "gw1";
    cfg.node_address = "10.0.0.1:2049";
    cfg.takeover = "manual";
    cfg.fence_lease_ms = 1000;
    core::FsOwnerView view;
    server::FsClusterController fc(cfg, exports, store, view, std::move(hooks), 7);
    server::CtlDeps deps{};
    deps.fs_cluster = &fc;
    auto ask = [&](const char* line) {
      return run_task(runtime, server::CtlServer::answer_async(deps, line));
    };

    // Before any tick: three unowned exports, the gateway line, the peers from the
    // node registry.
    EXPECT_STREQ(ask("cluster status"),
                 "mode=active-active node=gw1 node_epoch=7 node_address=10.0.0.1:2049 "
                 "shared_dir=/mnt/shared/.lightnfs-cluster peers=gw1,gw2 peers_alive=gw2 "
                 "takeover=manual migrations=0 exports=3\n"
                 "fsid=1 role=unowned nodes=gw1,gw2 owner=none address=- fs_epoch=0 "
                 "fence_age_ms=- fence_expires_in_ms=- grace_remaining_s=0 takeovers=0 "
                 "fence_lost=0 activation_failures=0\n"
                 "fsid=2 role=unowned nodes=gw1,gw2 owner=none address=- fs_epoch=0 "
                 "fence_age_ms=- fence_expires_in_ms=- grace_remaining_s=0 takeovers=0 "
                 "fence_lost=0 activation_failures=0\n"
                 "fsid=3 role=unowned nodes=gw2 owner=none address=- fs_epoch=0 "
                 "fence_age_ms=- fence_expires_in_ms=- grace_remaining_s=0 takeovers=0 "
                 "fence_lost=0 activation_failures=0\n");
    auto js = ask("cluster status --json");
    EXPECT_TRUE(js.find("{\"mode\":\"active-active\",\"node\":\"gw1\",\"node_epoch\":7,"
                        "\"node_address\":\"10.0.0.1:2049\","
                        "\"shared_dir\":\"/mnt/shared/.lightnfs-cluster\","
                        "\"peers\":[\"gw1\",\"gw2\"],\"peers_alive\":[\"gw2\"],"
                        "\"takeover\":\"manual\",\"migrations\":0,\"exports\":["
                        "{\"fsid\":1,\"role\":\"unowned\",\"nodes\":[\"gw1\",\"gw2\"],"
                        "\"owner\":null,\"address\":null,"
                        "\"fs_epoch\":0,\"fence_age_ms\":null,\"fence_expires_in_ms\":null,"
                        "\"grace_remaining_s\":0,\"takeovers\":0,\"fence_lost\":0,"
                        "\"activation_failures\":0},{\"fsid\":2,") != std::string::npos);
    EXPECT_TRUE(server::CtlServer::answer(deps, "status").find("role=active-active") !=
                std::string::npos);
    // The failover forms without an fsid, a bad fsid, an unknown one, bad subcommands.
    EXPECT_STREQ(ask("cluster takeover"), "cluster: fsid required\n");
    EXPECT_STREQ(ask("cluster standby --json"), "{\"error\":\"fsid required\"}\n");
    EXPECT_STREQ(ask("cluster takeover abc"), "cluster: fsid required\n");
    EXPECT_STREQ(ask("cluster takeover 9"), "cluster: unknown fsid 9\n");
    EXPECT_STREQ(ask("cluster standby 9 --json"), "{\"error\":\"unknown fsid 9\"}\n");
    EXPECT_STREQ(ask("cluster bogus"),
                 "cluster: expected status|exports [<node>]|takeover <fsid> [--force]|standby "
                 "<fsid>|migrate <fsid> <node>\n");
    // exports: nobody serves anything yet; us (not heartbeating), registered gw2, a node
    // only named in an export list, and a stranger.
    EXPECT_STREQ(ask("cluster exports"),
                 "node=gw1 alive=no address=10.0.0.1:2049 exports=0 fsids=-\n");
    EXPECT_STREQ(ask("cluster exports gw2 --json"),
                 "{\"node\":\"gw2\",\"alive\":true,\"address\":\"10.0.0.2:2049\",\"fsids\":[],"
                 "\"exports\":[]}\n");
    EXPECT_STREQ(ask("cluster exports gw9"),
                 "cluster: unknown node gw9 (not registered, not in any export's nodes)\n");
    EXPECT_STREQ(ask("cluster exports gw9 --json"),
                 "{\"error\":\"unknown node gw9 (not registered, not in any export's nodes)\"}\n");
    EXPECT_STREQ(ask("cluster migrate 1"), "cluster: fsid and node required\n");
    EXPECT_STREQ(ask("cluster migrate --json"), "{\"error\":\"fsid and node required\"}\n");
    EXPECT_STREQ(ask("cluster migrate 9 gw2"), "cluster: unknown fsid 9\n");
    EXPECT_STREQ(ask("cluster migrate 1 gw2"),
                 "cluster: fsid 1 not active here (role=unowned, owner=nobody)\n");
    EXPECT_STREQ(ask("cluster --json"), "{\"error\":\"bad subcommand\"}\n");

    // gw2 holds F2 live (owner record written) and F3; manual policy: one tick only
    // refreshes the view — F2/F3 remote with gw2's address and fs epoch, F1 unowned.
    store.fs_taken_by(2, "gw2", 5);
    store.fs_epochs[2] = 5;
    (void)store.put_owner(2, {"gw2", "10.0.0.2:2049", 5});
    store.fs_taken_by(3, "gw2", 2);
    store.fs_epochs[3] = 2;
    fc.tick();
    auto st = ask("cluster status");
    EXPECT_TRUE(st.find("fsid=1 role=unowned nodes=gw1,gw2 owner=none") != std::string::npos);
    EXPECT_TRUE(st.find("fsid=2 role=remote nodes=gw1,gw2 owner=gw2 address=10.0.0.2:2049 "
                        "fs_epoch=5 fence_age_ms=") != std::string::npos);
    EXPECT_TRUE(st.find("fsid=3 role=remote nodes=gw2 owner=gw2 address=10.0.0.2:2049 "
                        "fs_epoch=2 ") != std::string::npos);
    EXPECT_TRUE(ask("cluster status --json")
                    .find("\"fsid\":2,\"role\":\"remote\",\"nodes\":[\"gw1\",\"gw2\"],"
                          "\"owner\":\"gw2\",\"address\":\"10.0.0.2:2049\",\"fs_epoch\":5,"
                          "\"fence_age_ms\":") != std::string::npos);
    EXPECT_STREQ(ask("cluster migrate 2 gw1 --json"),
                 "{\"error\":\"fsid 2 not active here (role=remote, owner=gw2)\"}\n");
    // exports gw2: the two it holds, with paths, as this gateway sees them.
    EXPECT_STREQ(ask("cluster exports gw2"),
                 "node=gw2 alive=yes address=10.0.0.2:2049 exports=2 fsids=2,3\n"
                 "fsid=2 path=/export/2 role=remote fs_epoch=5\n"
                 "fsid=3 path=/export/3 role=remote fs_epoch=2\n");
    EXPECT_STREQ(ask("cluster exports gw2 --json"),
                 "{\"node\":\"gw2\",\"alive\":true,\"address\":\"10.0.0.2:2049\",\"fsids\":[2,3],"
                 "\"exports\":[{\"fsid\":2,\"path\":\"/export/2\",\"role\":\"remote\","
                 "\"fs_epoch\":5},{\"fsid\":3,\"path\":\"/export/3\",\"role\":\"remote\","
                 "\"fs_epoch\":2}]}\n");

    // takeover 1: free fence, no force needed; the hook runs, the row flips to active.
    EXPECT_STREQ(ask("cluster takeover 1"),
                 "takeover started: fsid=1 node=gw1 fs_epoch=1 role=active\n");
    EXPECT_STREQ(joined_calls(calls), "activate:1");
    EXPECT_TRUE(fc.role_of(1) == server::Role::kActive);
    st = ask("cluster status");
    EXPECT_TRUE(st.find("fsid=1 role=active nodes=gw1,gw2 owner=gw1 address=10.0.0.1:2049 "
                        "fs_epoch=1 fence_age_ms=") != std::string::npos);
    EXPECT_TRUE(st.find(" grace_remaining_s=0 takeovers=1 fence_lost=0 activation_failures=0\n") !=
                std::string::npos);
    EXPECT_STREQ(ask("cluster takeover 1"), "cluster: fsid 1 not remote (role=active)\n");
    // exports (ours): the export we just took, active; taking it wrote our heartbeat.
    EXPECT_STREQ(ask("cluster exports gw1"),
                 "node=gw1 alive=yes address=10.0.0.1:2049 exports=1 fsids=1\n"
                 "fsid=1 path=/export/1 role=active fs_epoch=1\n");
    // migrate 1: to ourselves, to a stranger, to a dead peer — refused; to live gw2 —
    // the owner record names it and the export drains (plan 12 D1).
    EXPECT_STREQ(ask("cluster migrate 1 gw1"),
                 "cluster: cannot migrate fsid 1 to gw1: that is this node\n");
    EXPECT_STREQ(ask("cluster migrate 1 gw9 --json"),
                 "{\"error\":\"node gw9 is not a live gateway (no registration or heartbeat)\"}\n");
    store.age_out_node("gw2", 1000);
    EXPECT_STREQ(ask("cluster migrate 1 gw2"),
                 "cluster: node gw2 is not a live gateway (no registration or heartbeat)\n");
    (void)store.renew_fences("gw2", std::chrono::seconds(60));
    EXPECT_STREQ(ask("cluster migrate 1 gw2 --json"),
                 "{\"migrate\":true,\"fsid\":1,\"to\":\"gw2\"}\n");
    EXPECT_STREQ(joined_calls(calls), "activate:1 deactivate:1");
    EXPECT_TRUE(fc.role_of(1) == server::Role::kStandby);
    EXPECT_STREQ(store.owners[1].node, "gw2");
    st = ask("cluster status");
    EXPECT_TRUE(st.find("migrations=1 exports=3") != std::string::npos);
    EXPECT_TRUE(st.find("fsid=1 role=remote nodes=gw1,gw2 owner=gw2 address=10.0.0.2:2049 "
                        "fs_epoch=1 fence_age_ms=- fence_expires_in_ms=-") != std::string::npos);
    EXPECT_STREQ(ask("cluster migrate 1 gw2"),
                 "cluster: fsid 1 not active here (role=remote, owner=gw2)\n");
    // Back for the rest of the test: gw2 took it and released it again; gw1 retakes.
    store.fs_taken_by(1, "gw2", 2);
    store.fs_epochs[1] = 2;
    (void)store.put_owner(1, {"gw2", "10.0.0.2:2049", 2});
    fc.tick();
    ASSERT_TRUE(store.release_fs_fence(1, "gw2").has_value());
    fc.tick();
    EXPECT_STREQ(ask("cluster takeover 1"),
                 "takeover started: fsid=1 node=gw1 fs_epoch=3 role=active\n");
    calls.clear();
    EXPECT_STREQ(ask("cluster takeover 1 --json"),
                 "{\"error\":\"fsid 1 not remote (role=active)\"}\n");
    // takeover 2: gw2's fence is live — refused naming gw2; --force evicts it (JSON
    // success carries the new fs epoch).
    EXPECT_STREQ(ask("cluster takeover 2"),
                 "cluster: fsid 2 fence held by gw2 (retry with --force to take it)\n");
    EXPECT_STREQ(
        ask("cluster takeover 2 --force --json"),
        "{\"takeover\":true,\"fsid\":2,\"forced\":true,\"fs_epoch\":6,\"role\":\"active\"}\n");
    EXPECT_STREQ(joined_calls(calls), "activate:2");
    EXPECT_TRUE(store.fences["gw2"].holds.size() == 1u);  // F3 only
    // takeover 3: not in F3's node list, but ctl --force is the operator's call; without
    // force the live fence is refused just the same.
    EXPECT_STREQ(ask("cluster takeover 3"),
                 "cluster: fsid 3 fence held by gw2 (retry with --force to take it)\n");

    // standby 1: drained and released; standby again is refused with the role.
    EXPECT_STREQ(ask("cluster standby 1 --json"), "{\"standby\":true,\"fsid\":1}\n");
    EXPECT_STREQ(joined_calls(calls), "activate:2 deactivate:1");
    EXPECT_TRUE(fc.role_of(1) == server::Role::kStandby);
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 1u);  // F2 only
    EXPECT_STREQ(ask("cluster standby 1"), "cluster: fsid 1 not active (role=unowned)\n");
    EXPECT_STREQ(ask("cluster standby 2"), "standby requested: fsid=2 draining\n");
    EXPECT_STREQ(ask("cluster standby 3 --json"),
                 "{\"error\":\"fsid 3 not active (role=remote)\"}\n");
    // A store that cannot list nodes still answers, with the peers unknown.
    store.fail_list = errno_from(EIO);
    EXPECT_TRUE(ask("cluster status").find(" peers=? ") != std::string::npos);
    EXPECT_TRUE(ask("cluster status --json").find("\"peers\":null") != std::string::npos);
  }
  runtime.stop_and_join();
}

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
