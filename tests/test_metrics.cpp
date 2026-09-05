// obs/metrics.cpp exposition coverage (plan doc 10 §7.1): ExportMetrics, the v3 series
// (including the calls==0 skip rule), flat totals, registered text providers, and the
// span overload of append_histogram.  Metrics::instance() is process-global, so every
// assertion is delta-based or presence-based rather than assuming pristine counters.

#include <string>
#include <thread>
#include <vector>

#include "mem_cluster_store.hpp"
#include "mini_test.hpp"
#include "obs/metrics.hpp"
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
  const uint32_t bumped = 20;   // PATHCONF — unused by other suites in this binary
  const uint32_t maybe_zero = 21;  // COMMIT
  uint64_t before = m.v3_calls[bumped].load();
  m.v3_calls[bumped].fetch_add(2);
  m.v3_errors[bumped].fetch_add(1);
  m.v3_duration_us[bumped].fetch_add(150);
  m.v3_duration[bumped].observe_us(150);
  auto text = obs::prometheus_text();
  std::string label = std::string("proc=\"") + obs::v3_proc_name(bumped) + "\"";
  EXPECT_EQ(sample_value(text, "lightnfs_v3_calls_total{" + label + "}"),
            static_cast<long long>(before + 2));
  EXPECT_TRUE(text.find("lightnfs_v3_errors_total{" + label + "}") != std::string::npos);
  EXPECT_TRUE(text.find("lightnfs_v3_duration_seconds_bucket{" + label + "") !=
              std::string::npos);
  // calls == 0 → the proc renders no series at all.
  std::string other = std::string("proc=\"") + obs::v3_proc_name(maybe_zero) + "\"";
  bool has_calls = m.v3_calls[maybe_zero].load() != 0;
  EXPECT_EQ(text.find("lightnfs_v3_calls_total{" + other + "}") != std::string::npos,
            has_calls);
}

TEST(Metrics, FlatTotalsRenderCurrentValues) {
  auto& m = obs::Metrics::instance();
  m.rpc_garbage.fetch_add(3);
  m.conns_rejected.fetch_add(2);
  m.read_bytes.fetch_add(1234);
  m.conns_active.fetch_add(2);
  m.conns_active.fetch_sub(1);
  auto text = obs::prometheus_text();
  EXPECT_EQ(sample_value(text, "lightnfs_rpc_garbage_total"),
            static_cast<long long>(m.rpc_garbage.load()));
  EXPECT_EQ(sample_value(text, "lightnfs_connections_rejected_total"),
            static_cast<long long>(m.conns_rejected.load()));
  EXPECT_EQ(sample_value(text, "lightnfs_read_bytes_total"),
            static_cast<long long>(m.read_bytes.load()));
  EXPECT_EQ(sample_value(text, "lightnfs_connections_active"),
            static_cast<long long>(m.conns_active.load()));
}

// plan 10 C4: the cluster controller's own text provider — role one-hot, epoch, fence
// ownership/age, takeover counters and the activation histogram — registered for the
// controller's lifetime and gone with it.
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
    server::ClusterController ctl(cfg, store, {});  // inline hooks, nothing to run
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
    EXPECT_TRUE(text.find("# TYPE lightnfs_cluster_activation_seconds histogram\n") !=
                std::string::npos);
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
    EXPECT_TRUE(text.find("lightnfs_cluster_activation_seconds_bucket{le=\"0.0001\"} 1\n") !=
                std::string::npos);
    EXPECT_TRUE(text.find("lightnfs_cluster_activation_seconds_bucket{le=\"+Inf\"} 1\n") !=
                std::string::npos);

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
  EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"0.001\"} 1\n") !=
              std::string::npos);
  EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"0.002\"} 2\n") !=
              std::string::npos);
  EXPECT_TRUE(out.find("lightnfs_test_hist_bucket{op=\"X\",le=\"+Inf\"} 3\n") !=
              std::string::npos);
  EXPECT_TRUE(out.find("lightnfs_test_hist_count{op=\"X\"} 3\n") != std::string::npos);
  EXPECT_TRUE(out.find("lightnfs_test_hist_sum{op=\"X\"} 0.0045\n") != std::string::npos);
}
