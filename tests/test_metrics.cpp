// obs/metrics.cpp exposition coverage (plan doc 10 §7.1): ExportMetrics, the v3 series
// (including the calls==0 skip rule), flat totals, registered text providers, and the
// span overload of append_histogram.  Metrics::instance() is process-global, so every
// assertion is delta-based or presence-based rather than assuming pristine counters.

#include <stdlib.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "backend/memory/memory.hpp"
#include "core/config.hpp"
#include "core/fs_owner_view.hpp"
#include "mem_cluster_store.hpp"
#include "mini_test.hpp"
#include "obs/metrics.hpp"
#include "server/catalog_applier.hpp"
#include "server/catalog_boot.hpp"
#include "server/cluster_controller.hpp"

using namespace lnfs;

namespace {

// Value of the first sample line "name{labels} value" whose name-part starts with
// `series` (exact match up to '{' or ' '); -1 when absent.
long long sample_value(const std::string& text, const std::string& series) {
    size_t pos = 0;
    while ((pos = text.find(series, pos)) != std::string::npos) {
        bool line_start = pos == 0 || text[pos - 1] == '\n';
        char next = pos + series.size() < text.size() ? text[pos + series.size()] : '\0';
        if (line_start && (next == ' ' || next == '{')) {
            size_t sp = text.find(' ', pos);
            if (sp == std::string::npos) return -1;
            return std::stoll(text.substr(sp + 1));
        }
        pos += series.size();
    }
    return -1;
}

}  // namespace

TEST(Metrics, ExportMetricsCountersSumAcrossThreads) {
    obs::ExportMetrics m;
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
        workers.emplace_back([&m] {
            for (int i = 0; i < 1000; ++i) {
                m.read_bytes.fetch_add(3);
                m.write_bytes.fetch_add(5);
                m.read_ops.fetch_add(1);
                m.write_ops.fetch_add(1);
            }
        });
    for (auto& w : workers) w.join();
    EXPECT_EQ(m.read_bytes.load(), 12000u);
    EXPECT_EQ(m.write_bytes.load(), 20000u);
    EXPECT_EQ(m.read_ops.load(), 4000u);
    EXPECT_EQ(m.write_ops.load(), 4000u);
}

TEST(Metrics, TextProviderAppendsToExposition) {
    // Providers are append-only for the life of the process; a static flag keeps the
    // callback safe for every later prometheus_text() call in this binary.
    static std::atomic<uint64_t> value{41};
    obs::register_text_provider([](std::string& out) {
        out += "lightnfs_test_provider_gauge ";
        out += std::to_string(value.load());
        out += "\n";
    });
    value.store(42);
    auto text = obs::prometheus_text();
    EXPECT_EQ(sample_value(text, "lightnfs_test_provider_gauge"), 42);
    // Providers run on every render, reflecting current state.
    value.store(43);
    EXPECT_EQ(sample_value(obs::prometheus_text(), "lightnfs_test_provider_gauge"), 43);
}

TEST(Metrics, V3SeriesRenderAndZeroSkipRule) {
    auto& m = obs::Metrics::instance();
    // Pick two v3 procs; bump one, leave the other at whatever the process has seen.
    // PATHCONF — unused by other suites in this binary
    const uint32_t bumped = 20;
    // COMMIT
    const uint32_t maybe_zero = 21;
    uint64_t before = m.v3_calls[bumped].load();
    m.v3_calls[bumped].fetch_add(2);
    m.v3_errors[bumped].fetch_add(1);
    m.v3_duration_us[bumped].fetch_add(150);
    m.v3_duration[bumped].observe_us(150);
    auto text = obs::prometheus_text();
    std::string label = std::string("proc=\"") + obs::v3_proc_name(bumped) + "\"";
    EXPECT_EQ(sample_value(text, "lightnfs_v3_calls_total{" + label + "}"), static_cast<long long>(before + 2));
    EXPECT_TRUE(text.find("lightnfs_v3_errors_total{" + label + "}") != std::string::npos);
    EXPECT_TRUE(text.find("lightnfs_v3_duration_seconds_bucket{" + label + "") != std::string::npos);
    // calls == 0 → the proc renders no series at all.
    std::string other = std::string("proc=\"") + obs::v3_proc_name(maybe_zero) + "\"";
    bool has_calls = m.v3_calls[maybe_zero].load() != 0;
    EXPECT_EQ(text.find("lightnfs_v3_calls_total{" + other + "}") != std::string::npos, has_calls);
}

TEST(Metrics, FlatTotalsRenderCurrentValues) {
    auto& m = obs::Metrics::instance();
    m.rpc_garbage.fetch_add(3);
    m.conns_rejected.fetch_add(2);
    m.read_bytes.fetch_add(1234);
    m.conns_active.fetch_add(2);
    m.conns_active.fetch_sub(1);
    auto text = obs::prometheus_text();
    EXPECT_EQ(sample_value(text, "lightnfs_rpc_garbage_total"), static_cast<long long>(m.rpc_garbage.load()));
    EXPECT_EQ(sample_value(text, "lightnfs_connections_rejected_total"),
              static_cast<long long>(m.conns_rejected.load()));
    EXPECT_EQ(sample_value(text, "lightnfs_read_bytes_total"), static_cast<long long>(m.read_bytes.load()));
    EXPECT_EQ(sample_value(text, "lightnfs_connections_active"), static_cast<long long>(m.conns_active.load()));
}

// plan 10 C4: the cluster controller's own text provider — role one-hot, epoch, fence
// ownership/age, takeover counters and the activation histogram — registered for the
// controller's lifetime and gone with it.
// Active-active (plan 12 C4): per-export series from the FsClusterController — role
// one-hot, the owner sample only while somebody holds the export, fs epoch and the
// per-export counters, plus the node epoch — for the controller's lifetime.
TEST(Metrics, ClusterFsSeries) {
    const size_t providers_before = obs::text_provider_count();
    {
        test::MemClusterStore store;
        (void)store.put_node_address("gw2", "10.0.0.2:2049");
        core::ExportTable exports;
        for (uint32_t fsid = 1; fsid <= 3; ++fsid) {
            core::ExportConfig ec;
            ec.path = "/export/" + std::to_string(fsid);
            ec.fsid = fsid;
            ec.clients = {"127.0.0.0/8"};
            ec.nodes = fsid == 3 ? std::vector<std::string>{"gw2", "gw1"} : std::vector<std::string>{"gw1", "gw2"};
            ASSERT_TRUE(exports.add(ec, std::make_unique<backend::MemoryBackend>(fsid)).has_value());
        }
        core::ClusterConfig cfg;
        cfg.enabled = true;
        cfg.mode = "active-active";
        cfg.id = "cluster-metrics-test";
        cfg.node = "gw1";
        cfg.node_address = "10.0.0.1:2049";
        cfg.fence_lease_ms = 1000;
        core::FsOwnerView view;
        server::FsClusterController ctl(cfg, exports, store, view, {}, 3);
        EXPECT_EQ(obs::text_provider_count(), providers_before + 1);

        // Nothing ticked: every export unowned, no owner samples, zero counters.
        auto text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_node_epoch"), 3);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_migrations_total"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"unowned\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"active\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"3\",role=\"remote\"}"), 0);
        EXPECT_TRUE(text.find("lightnfs_cluster_fs_owner{") == std::string::npos);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_epoch{fsid=\"2\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_takeovers_total{fsid=\"2\"}"), 0);

        // gw2 holds F3 live; one tick takes F1/F2 (ours, first in line) and leaves F3
        // remote: two active, one remote, owners for all three, epochs minted.
        store.fs_taken_by(3, "gw2", 4);
        store.fs_epochs[3] = 4;
        (void)store.put_owner(3, {"gw2", "10.0.0.2:2049", 4});
        ctl.tick();
        ASSERT_TRUE(ctl.role_of(1) == server::Role::kActive);
        ASSERT_TRUE(ctl.role_of(2) == server::Role::kActive);
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"active\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"unowned\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"2\",role=\"active\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"3\",role=\"remote\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"3\",role=\"active\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"1\",node=\"gw1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"2\",node=\"gw1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"3\",node=\"gw2\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"3\",node=\"gw1\"}"), -1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_epoch{fsid=\"1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_epoch{fsid=\"3\"}"), 4);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_takeovers_total{fsid=\"1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_takeovers_total{fsid=\"3\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_fence_lost_total{fsid=\"1\"}"), 0);

        // gw2 takes F1 by force: F1 drains (fence lost, epoch 0 for us) and is remote to
        // gw2; gw2 then dies — F1 unowned, no owner sample for it.
        store.fs_taken_by(1, "gw2", 9);
        ctl.tick();
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"remote\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"1\",node=\"gw2\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_epoch{fsid=\"1\"}"), 9);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_fence_lost_total{fsid=\"1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_fence_lost_total{fsid=\"2\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"2\",role=\"active\"}"), 1);
        store.age_out_node("gw2", 1000);
        // F1 is ours again (first in line), F3 as well (gw2 gone)
        ctl.tick();
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"1\",role=\"active\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_takeovers_total{fsid=\"1\"}"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"3\",role=\"active\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"3\",node=\"gw1\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"3\",node=\"gw2\"}"), -1);
        // A planned migration counts (plan 12 D1): F3 handed to gw2 (registered, alive).
        (void)store.renew_fences("gw2", std::chrono::seconds(60));
        ASSERT_TRUE(ctl.request_migrate(3, "gw2").has_value());
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_migrations_total"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"3\",role=\"remote\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"3\",node=\"gw2\"}"), 1);
        // An export released by the operator sits unowned: role one-hot says so, no owner.
        ASSERT_TRUE(ctl.request_release(2).has_value());
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_role{fsid=\"2\",role=\"unowned\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_owner{fsid=\"2\",node=\"gw1\"}"), -1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fs_epoch{fsid=\"2\"}"), 0);
    }
    EXPECT_EQ(obs::text_provider_count(), providers_before);
    EXPECT_EQ(sample_value(obs::prometheus_text(), "lightnfs_cluster_node_epoch"), -1);
}

// plan 12 C3: the catalog follower's series, registered for the applier's lifetime
// (the same set under failover and active-active): the applied and the latest seen
// version, the pending flag, and the apply / failure counters.
TEST(Metrics, CatalogSeries) {
    const size_t providers_before = obs::text_provider_count();
    char tmpl[] = "/tmp/lnfs-metcat-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    auto doc = [&](uint64_t v, const std::string& extra) {
        return "[catalog]\nversion = " + std::to_string(v) + "\n[[export]]\npath = \"" + dir +
               "/a\"\nfsid = 1\nbackend = \"local\"\nclients = [\"127.0.0.0/8\"]\n" + extra;
    };
    auto block = [&](uint32_t fsid, const std::string& path) {
        return "[[export]]\npath = \"" + path + "\"\nfsid = " + std::to_string(fsid) +
               "\nbackend = \"local\"\nclients = [\"127.0.0.0/8\"]\n";
    };
    {
        test::MemClusterStore store;
        store.catalog_docs[1] = doc(1, "");
        auto local = core::parse_config(
            "[cluster]\nenabled = true\nid = \"cluster-metrics-test\"\nshared_dir = \"/srv/shared\"\n"
            "node = \"gw1\"\nexports_source = \"catalog\"\ncatalog_refresh = \"manual\"\n");
        ASSERT_TRUE(local.has_value());
        auto boot = server::load_catalog_exports(store, *local);
        ASSERT_TRUE(boot.has_value());
        auto table = core::ExportTable::build(*local);
        ASSERT_TRUE(table.has_value());
        local->exports.clear();
        local->exports_from_catalog = false;
        server::CatalogApplier applier({.store = store, .exports = **table, .local = *local, .node = "gw1"},
                                       boot->version, boot->catalog, boot->digest);
        EXPECT_EQ(obs::text_provider_count(), providers_before + 1);

        // Booted from v1: applied = latest = 1, nothing pending, no applies yet.
        auto text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_version"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_latest_version"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_pending"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_applies_total"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_apply_failures_total"), 0);

        // v2 published, seen by a manual-mode poll: latest 2, pending, still serving v1.
        ASSERT_TRUE(store.write_catalog(1, doc(2, block(2, dir + "/b"))).has_value());
        applier.poll();
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_version"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_latest_version"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_pending"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_applies_total"), 0);

        // Applied: version 2, one apply, pending cleared.
        ASSERT_TRUE(applier.apply_latest().has_value());
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_version"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_latest_version"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_pending"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_applies_total"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_apply_failures_total"), 0);

        // v3 this host cannot serve: a failure counted, v3 seen and pending, v2 served.
        ASSERT_TRUE(store.write_catalog(2, doc(3, block(9, "/nonexistent/x"))).has_value());
        EXPECT_FALSE(applier.apply_latest().has_value());
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_version"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_latest_version"), 3);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_pending"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_applies_total"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_apply_failures_total"), 1);

        // The catalog gone from the store: latest 0, nothing pending, v2 still served.
        store.catalog_docs.clear();
        applier.poll();
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_version"), 2);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_latest_version"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_pending"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_catalog_apply_failures_total"), 1);
    }
    EXPECT_EQ(obs::text_provider_count(), providers_before);
    EXPECT_EQ(sample_value(obs::prometheus_text(), "lightnfs_cluster_catalog_version"), -1);
    std::filesystem::remove_all(dir);
}

TEST(Metrics, ClusterSeriesRenderWithControllerLifetime) {
    const size_t providers_before = obs::text_provider_count();
    {
        test::MemClusterStore store;
        store.epoch = 4;
        core::ClusterConfig cfg;
        cfg.enabled = true;
        cfg.id = "cluster-metrics-test";
        cfg.node = "gw1";
        cfg.fence_lease_ms = 1000;
        // inline hooks, nothing to run
        server::ClusterController ctl(cfg, store, {});
        EXPECT_EQ(obs::text_provider_count(), providers_before + 1);

        // Standby, nothing seen yet: role one-hot, no fence age sample, zero counters.
        auto text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"standby\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"activating\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"active\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"draining\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_epoch"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_owned"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_age_seconds"), -1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_takeovers_total"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_lost_total"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_activation_failures_total"), 0);
        EXPECT_TRUE(text.find("# TYPE lightnfs_cluster_activation_seconds histogram\n") != std::string::npos);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_activation_seconds_count"), 0);

        // One takeover (free fence): active, epoch minted, our fence with an age, one
        // observation in the histogram (activation is far under the first bound).
        ctl.tick();
        ASSERT_TRUE(ctl.role() == server::Role::kActive);
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"standby\"}"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"active\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_epoch"), 5);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_owned"), 1);
        EXPECT_TRUE(text.find("\nlightnfs_cluster_fence_age_seconds 0.") != std::string::npos);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_takeovers_total"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_activation_seconds_count"), 1);
        EXPECT_TRUE(text.find("lightnfs_cluster_activation_seconds_bucket{le=\"0.0001\"} 1\n") != std::string::npos);
        EXPECT_TRUE(text.find("lightnfs_cluster_activation_seconds_bucket{le=\"+Inf\"} 1\n") != std::string::npos);

        // Fence taken by another node: draining → standby, fence_lost counted, the record
        // seen is theirs (not owned), epoch back to 0.
        store.taken_by("gw2", 9);
        ctl.tick();
        ASSERT_TRUE(ctl.role() == server::Role::kStandby);
        text = obs::prometheus_text();
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_role{role=\"standby\"}"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_owned"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_fence_lost_total"), 1);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_epoch"), 0);
        EXPECT_EQ(sample_value(text, "lightnfs_cluster_takeovers_total"), 1);
    }
    EXPECT_EQ(obs::text_provider_count(), providers_before);
    EXPECT_EQ(sample_value(obs::prometheus_text(), "lightnfs_cluster_epoch"), -1);
}

TEST(Metrics, AppendHistogramSpanOverload) {
    // Synthetic two-bucket histogram: bounds 1000us and 2000us, one observation each and
    // one overflow; cumulative counts must be 1, 2, 3 with sum in seconds.
    const uint64_t bounds[] = {1000, 2000};
    const uint64_t buckets[] = {1, 1, 1};
    std::string out;
    obs::append_histogram(out, "lightnfs_test_hist", "op=\"X\"", bounds, buckets,
                          /*sum_us=*/4500);
    EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"0.001\"} 1\n") != std::string::npos);
    EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"0.002\"} 2\n") != std::string::npos);
    EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"+Inf\"} 3\n") != std::string::npos);
    EXPECT_TRUE(out.find("lightnfs_test_hist_count{op=\"X\"} 3\n") != std::string::npos);
    EXPECT_TRUE(out.find("lightnfs_test_hist_sum{op=\"X\"} 0.0045\n") != std::string::npos);
}
