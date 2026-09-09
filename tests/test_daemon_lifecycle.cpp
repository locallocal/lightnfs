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
#include "core/catalog.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "mem_cluster_store.hpp"
#include "obs/metrics.hpp"
#include "runtime/runtime.hpp"
#include "server/catalog_boot.hpp"
#include "server/data_plane.hpp"
#include "server/main_loop.hpp"
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
    return core;
}

}  // namespace

TEST(DaemonLifecycle, ActivateDeactivateTwiceOverOneRuntime) {
    char tmpl[] = "/tmp/lnfs-dl-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    core::ServerConfig cfg;
    cfg.state_dir = dir;
    cfg.ctl_socket = dir + "/ctl.sock";
    // ephemeral loopback ports
    cfg.port = 0;
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
        // ctl accept loop up
        std::this_thread::sleep_for(50ms);
        EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=standby") != std::string::npos);
        EXPECT_STREQ(ctl(cfg.ctl_socket, "drc"), "not active\n");

        for (int round = 0; round < 2; ++round) {
            auto core = make_core(100 + static_cast<uint64_t>(round));
            auto plane = server::activate(cfg, cluster, core, runtime, mgmt);
            ASSERT_TRUE(plane.has_value());
            ASSERT_TRUE(plane->frontend.has_value());
            EXPECT_TRUE(plane->stack->nfs4.has_value());
            EXPECT_EQ(plane->stack->state.config().boot_epoch, 100u + static_cast<uint64_t>(round));
            // 4 groups + v4 moved + pool
            EXPECT_EQ(obs::text_provider_count(), providers_before + 6);
            // ctl now addresses this plane; the epoch shows in the state dump.
            EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=active") != std::string::npos);
            EXPECT_TRUE(ctl(cfg.ctl_socket, "state").find("boot_epoch=" + std::to_string(100 + round)) !=
                        std::string::npos);
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
            // 100 ms grace + close; not the lease scanner's 1 s
            EXPECT_TRUE(took < 2s);
            EXPECT_TRUE(plane->stack == nullptr);
            EXPECT_FALSE(plane->frontend.has_value());
            EXPECT_EQ(transport::ConnRegistry::instance().count(), conns_before);
            // nothing leaked
            EXPECT_EQ(obs::text_provider_count(), providers_before);
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
        // held: times out, plane pointer already cleared
        EXPECT_FALSE(slot.detach(20ms));
        EXPECT_TRUE(slot.load() == nullptr);
        // late comers see nothing
        EXPECT_FALSE(static_cast<bool>(slot.acquire()));
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

// ---- plan 12 C1: booting from the shared catalog ---------------------------------

namespace {

// This host's side of a catalog-mode configuration: [cluster] + [backend_defaults],
// no [[export]].
core::Config local_catalog_config(const std::string& extra = "") {
    auto parsed = core::parse_config(
        "[cluster]\nenabled = true\nid = \"cluster-c1\"\nshared_dir = \"/srv/shared\"\n"
        "node = \"gw1\"\nexports_source = \"catalog\"\n" +
        extra);
    if (!parsed) return {};
    return *parsed;
}

std::string catalog_text(uint64_t version, const std::string& exports) {
    return "[catalog]\nversion = " + std::to_string(version) + "\n" + exports;
}

}  // namespace

// A catalog in the store: its exports (merged with this host's defaults) become the
// table, catalog.<node> records the version, the peers' versions only warn.
TEST(DaemonLifecycle, CatalogBootFromStore) {
    char tmpl[] = "/tmp/lnfs-cb-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::filesystem::create_directories(dir + "/data");
    test::MemClusterStore store;
    store.catalog_docs[3] =
        catalog_text(3, "[[export]]\npath = \"" + dir +
                            "/data\"\nfsid = 7\nbackend = \"local\"\n"
                            "clients = [\"127.0.0.0/8\"]\nnodes = [\"gw1\", \"gw2\"]\nreadonly = true\n"
                            "[[export]]\npath = \"/export/ceph\"\nfsid = 9\nbackend = \"cephfs\"\n"
                            "clients = [\"10.0.0.0/8\"]\ndisabled = true\n[export.cephfs]\nfs_name = \"fs\"\n");
    auto local = local_catalog_config("[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\n");
    ASSERT_TRUE(local.cluster.enabled);
    std::string why;
    auto boot = server::load_catalog_exports(store, local, &why);
    ASSERT_TRUE(boot.has_value());
    EXPECT_TRUE(boot->present);
    EXPECT_EQ(boot->version, 3u);
    EXPECT_TRUE(boot->digest.starts_with("sha256:"));
    EXPECT_TRUE(local.exports_from_catalog);
    // the disabled cephfs export is not served
    ASSERT_TRUE(local.exports.size() == 1u);
    EXPECT_EQ(local.exports[0].fsid, 7u);
    EXPECT_TRUE(local.exports[0].readonly);
    EXPECT_STREQ(local.exports[0].nodes[1], "gw2");
    EXPECT_STREQ(boot->digest, core::canonical_exports_digest(local));
    // The merged config builds a table exactly like a local one.
    auto table = core::ExportTable::build(local);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ((*table)->size(), 1u);
    ASSERT_TRUE((*table)->by_fsid(7) != nullptr);
    EXPECT_STREQ((*table)->by_fsid(7)->path, dir + "/data");
    EXPECT_TRUE((*table)->by_fsid(7)->readonly.load());
    // Nothing was written while reading.
    EXPECT_TRUE(store.catalog_applied.empty());
    EXPECT_TRUE(store.digests.empty());

    // catalog.<node>: version, digest, "ok".
    server::record_catalog_applied(store, "gw1", boot->version, boot->digest, "ok");
    ASSERT_TRUE(store.catalog_applied.contains("gw1"));
    EXPECT_EQ(store.catalog_applied["gw1"].version, 3u);
    EXPECT_STREQ(store.catalog_applied["gw1"].digest, boot->digest);
    EXPECT_STREQ(store.catalog_applied["gw1"].status, "ok");
    EXPECT_TRUE(store.catalog_applied["gw1"].applied_at_ms > 0);
    // Peers behind, ahead or in error are reported, never refused; the digest is still
    // published for local-mode peers.
    (void)store.put_catalog_applied({"gw2", 2, "sha256:other", 1, "ok"});
    (void)store.put_catalog_applied({"gw3", 4, "sha256:other", 1, "error:no defaults"});
    EXPECT_TRUE(server::check_catalog_consistency(store, "gw1", 3, boot->digest));
    EXPECT_STREQ(store.digests["gw1"], boot->digest);
    // A local-mode peer with a different digest does not block a catalog gateway either.
    (void)store.put_exports_digest("gw4", "sha256:local-mode-peer");
    EXPECT_TRUE(server::check_catalog_consistency(store, "gw1", 3, boot->digest));
    std::filesystem::remove_all(dir);
}

// No catalog yet (a new cluster): an empty export table, a data plane with a bare
// pseudo root, and catalog.<node> = 0 so peers see the gateway is in catalog mode.
TEST(DaemonLifecycle, CatalogBootEmpty) {
    test::MemClusterStore store;
    auto local = local_catalog_config();
    std::string why;
    auto boot = server::load_catalog_exports(store, local, &why);
    ASSERT_TRUE(boot.has_value());
    EXPECT_FALSE(boot->present);
    EXPECT_EQ(boot->version, 0u);
    EXPECT_TRUE(local.exports.empty());
    EXPECT_FALSE(local.exports_from_catalog);
    // empty is allowed in catalog mode
    auto table = core::ExportTable::build(local);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ((*table)->size(), 0u);
    EXPECT_TRUE((*table)->snapshot()->pseudo->root()->children.empty());
    server::record_catalog_applied(store, "gw1", 0, boot->digest, "ok");
    EXPECT_EQ(store.catalog_applied["gw1"].version, 0u);
    EXPECT_TRUE(server::check_catalog_consistency(store, "gw1", 0, boot->digest));

    // The data plane comes up over the empty table and answers ctl with exports=0.
    char tmpl[] = "/tmp/lnfs-ce-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    core::ServerConfig cfg;
    cfg.state_dir = dir;
    cfg.ctl_socket = dir + "/ctl.sock";
    cfg.port = 0;
    cfg.mount_port = 0;
    cfg.bind = "127.0.0.1";
    cfg.rpcbind = false;
    cfg.state_shards = 1;
    core::ClusterConfig cluster;
    rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
    runtime.start();
    {
        server::Management mgmt = server::Management::start(cfg, runtime, {});
        std::this_thread::sleep_for(50ms);
        std::array<std::byte, 16> key{std::byte{4}};
        server::CoreState core{std::move(*table), core::FileHandleCodec::from_key_only(key), 5};
        core.local_config = local;
        core.catalog_exports = true;
        auto plane = server::activate(cfg, cluster, core, runtime, mgmt);
        ASSERT_TRUE(plane.has_value());
        EXPECT_TRUE(plane->stack->nfs4.has_value());
        EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("exports=0") != std::string::npos);
        EXPECT_TRUE(ctl(cfg.ctl_socket, "status").find("role=active") != std::string::npos);
        EXPECT_TRUE(server::deactivate(*plane, cfg, mgmt, 50ms, 2s));
        mgmt.stop();
    }
    runtime.stop_and_join();
    std::filesystem::remove_all(dir);
}

// A catalog this host cannot serve: the failure names the version, the fsid and the
// key (or the path) at fault, and nothing is served or recorded.
TEST(DaemonLifecycle, CatalogBootLocalMergeFails) {
    test::MemClusterStore store;
    store.catalog_docs[5] = catalog_text(5,
                                         "[[export]]\npath = \"/export/ceph\"\nfsid = 9\nbackend = \"cephfs\"\n"
                                         "clients = [\"10.0.0.0/8\"]\n[export.cephfs]\nfs_name = \"fs\"\n");
    // A cluster-wide key in this host's [backend_defaults.cephfs] (the parser refuses
    // one in the file, plan 12 A1; an in-memory config is how it can still show up).
    auto local = local_catalog_config("[backend_defaults.cephfs]\nconf = \"/etc/ceph/ceph.conf\"\n");
    ASSERT_TRUE(local.cluster.enabled);
    local.backend_defaults["cephfs"].values["fs_name"] = "other";
    std::string why;
    auto boot = server::load_catalog_exports(store, local, &why);
    ASSERT_TRUE(!boot.has_value());
    EXPECT_EQ(static_cast<int>(boot.error()), EINVAL);
    EXPECT_TRUE(why.find("catalog v5") != std::string::npos);
    EXPECT_TRUE(why.find("fsid=9") != std::string::npos);
    EXPECT_TRUE(why.find("\"fs_name\"") != std::string::npos);
    EXPECT_TRUE(why.find("[backend_defaults.cephfs]") != std::string::npos);
    EXPECT_TRUE(local.exports.empty());
    EXPECT_FALSE(local.exports_from_catalog);
    EXPECT_TRUE(store.catalog_applied.empty());

    // An export path that does not exist on this host: the fsid is named.
    store.catalog_docs[6] =
        catalog_text(6,
                     "[[export]]\npath = \"/nonexistent/lnfs-c1-test\"\nfsid = 11\nbackend = \"local\"\n"
                     "clients = [\"127.0.0.0/8\"]\n");
    auto plain = local_catalog_config();
    ASSERT_TRUE(plain.cluster.enabled);
    why.clear();
    boot = server::load_catalog_exports(store, plain, &why);
    ASSERT_TRUE(!boot.has_value());
    EXPECT_TRUE(why.find("catalog v6") != std::string::npos);
    EXPECT_TRUE(why.find("fsid=11") != std::string::npos);
    EXPECT_TRUE(why.find("/nonexistent/lnfs-c1-test") != std::string::npos);
    EXPECT_TRUE(plain.exports.empty());

    // A catalog that fails the cluster-level rules (duplicate fsid) is refused too.
    store.catalog_docs[7] =
        catalog_text(7,
                     "[[export]]\npath = \"/a\"\nfsid = 1\nbackend = \"local\"\nclients = [\"127.0.0.0/8\"]\n"
                     "[[export]]\npath = \"/b\"\nfsid = 1\nbackend = \"local\"\nclients = [\"127.0.0.0/8\"]\n");
    why.clear();
    boot = server::load_catalog_exports(store, plain, &why);
    ASSERT_TRUE(!boot.has_value());
    EXPECT_TRUE(why.find("catalog v7") != std::string::npos);
    EXPECT_TRUE(why.find("fsid=1") != std::string::npos);
    // And one whose text does not parse.
    store.catalog_docs[8] = "[catalog]\nversion = 8\n[[export]]\nfsid = \"x\"\n";
    why.clear();
    boot = server::load_catalog_exports(store, plain, &why);
    ASSERT_TRUE(!boot.has_value());
    EXPECT_TRUE(why.find("catalog v8") != std::string::npos);
    EXPECT_TRUE(why.find("does not parse") != std::string::npos);
    // The store unreadable: the error is the store's, not EINVAL.
    store.fail_read = errno_from(EIO);
    why.clear();
    boot = server::load_catalog_exports(store, plain, &why);
    ASSERT_TRUE(!boot.has_value());
    EXPECT_EQ(static_cast<int>(boot.error()), EIO);
    EXPECT_TRUE(why.find("cannot read the catalog") != std::string::npos);
}

// plan 12 C2: MainLoop::call runs the closure on the loop thread and hands the result
// back (the ctl `reload` path); a loop that is not running answers nullopt within the
// timeout, and the loop thread itself runs the closure inline.
TEST(DaemonLifecycle, MainLoopCallRunsOnTheLoopThread) {
    server::MainLoop loop;
    std::atomic<bool> stop{false};
    std::atomic<int> reloads{0};
    std::atomic<bool> reload_request{false};
    std::thread::id loop_thread;
    std::thread runner([&] {
        loop_thread = std::this_thread::get_id();
        loop.run([&] { return stop.load(); }, [&] { return reload_request.exchange(false); }, [&] { ++reloads; });
    });
    std::thread::id ran_on{};
    auto answer = loop.call(
        [&] {
            ran_on = std::this_thread::get_id();
            // From the loop thread a nested call runs inline (no self-wait).
            auto nested = loop.call([] { return std::string("nested"); }, 10ms);
            return std::string("ran:") + (nested ? *nested : "timeout");
        },
        2s);
    ASSERT_TRUE(answer.has_value());
    EXPECT_STREQ(*answer, "ran:nested");
    EXPECT_TRUE(ran_on == loop_thread);
    EXPECT_TRUE(loop.on_loop_thread() == false);
    // A SIGHUP-style request is served on the loop thread between items.
    reload_request = true;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (reloads.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(reloads.load(), 1);
    stop = true;
    runner.join();
    // Not running any more: the call times out; drain() runs what was queued.
    int ran_later = 0;
    auto late = loop.call(
        [&] {
            ++ran_later;
            return std::string("late");
        },
        50ms);
    EXPECT_FALSE(late.has_value());
    EXPECT_EQ(ran_later, 0);
    loop.drain();
    EXPECT_EQ(ran_later, 1);
}
