#include <string>

#include "mini_test.hpp"

#include <thread>
#include <vector>

#include "obs/metrics.hpp"
#include "obs/errlog.hpp"
#include "util/flags.hpp"
#include "util/result.hpp"
#include "util/small_vec.hpp"

using namespace lnfs;

TEST(Result, ValueAndError) {
  Result<int> ok = 42;
  EXPECT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, 42);
  Result<int> bad = Err(Errno::kGarbage);
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ((int)bad.error(), (int)Errno::kGarbage);
}

TEST(Result, MoveOnlyPayload) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
  auto p = std::move(r).value();
  EXPECT_EQ(*p, 7);
}

TEST(Result, VoidSpecialization) {
  Result<void> ok;
  EXPECT_TRUE(ok.has_value());
  Result<void> bad = Err(errno_from(EIO));
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(raw(bad.error()), EIO);
}

static Result<int> parse_positive(int x) {
  if (x < 0) return Err(errno_from(EINVAL));
  return x * 2;
}
static Result<int> chained(int x) {
  int v = LNFS_TRY(parse_positive(x));
  return v + 1;
}

TEST(Result, TryMacro) {
  EXPECT_EQ(*chained(5), 11);
  EXPECT_EQ(raw(chained(-1).error()), EINVAL);
}

enum class TE : uint32_t { kA = 1, kB = 2, kC = 4 };

TEST(Flags, Basics) {
  Flags<TE> f;
  EXPECT_FALSE(f.any());
  f.set(TE::kA).set(TE::kC);
  EXPECT_TRUE(f.has(TE::kA));
  EXPECT_FALSE(f.has(TE::kB));
  auto g = f | TE::kB;
  EXPECT_TRUE(g.has(TE::kB));
  g.clear(TE::kA);
  EXPECT_FALSE(g.has(TE::kA));
}

TEST(SmallVec, InlineAndSpill) {
  SmallVec<std::string, 2> v;
  v.push_back("a");
  v.push_back("b");
  v.push_back("c");  // spills to heap
  v.push_back("d");
  EXPECT_EQ(v.size(), 4u);
  EXPECT_STREQ(v[0], "a");
  EXPECT_STREQ(v[3], "d");
  SmallVec<std::string, 2> w = std::move(v);
  EXPECT_EQ(w.size(), 4u);
  EXPECT_STREQ(w[2], "c");
  w.clear();
  EXPECT_TRUE(w.empty());
}

TEST(SmallVec, CopySemantics) {
  SmallVec<int, 4> v;
  for (int i = 0; i < 10; ++i) v.push_back(i);
  SmallVec<int, 4> w = v;
  EXPECT_EQ(w.size(), 10u);
  EXPECT_EQ(w[9], 9);
  EXPECT_EQ(v[9], 9);
}

// Error-reply sampling ring (design 08 §8.2): shared by the v3 and v4 engines, so the
// `what` column is a caller-resolved name rather than a v3 procedure number.
TEST(ErrLog, RecordAndDump) {
  obs::record_error_reply("127.0.0.1:1024", "GETATTR", 0x1234, 70);
  obs::record_error_reply("127.0.0.1:1024", "OPEN", 0x1235, 10011);
  auto out = obs::dump_error_replies();
  EXPECT_TRUE(out.find("proc=GETATTR") != std::string::npos);
  EXPECT_TRUE(out.find("proc=OPEN") != std::string::npos);
  EXPECT_TRUE(out.find("xid=0x1235") != std::string::npos);
  EXPECT_TRUE(out.find("status=10011") != std::string::npos);
}

TEST(ErrLog, RingKeepsMostRecent) {
  for (uint32_t i = 0; i < 100; ++i)
    obs::record_error_reply("peer", "COMPOUND", 0x9000 + i, 1);
  auto out = obs::dump_error_replies();
  EXPECT_TRUE(out.find("xid=0x9063") != std::string::npos);   // newest (i=99)
  EXPECT_TRUE(out.find("xid=0x9024") != std::string::npos);   // oldest kept (i=36)
  EXPECT_TRUE(out.find("xid=0x9023") == std::string::npos);   // evicted
}

// Ring capacity is configurable (plan doc 10 §3.7) and the `what` field holds the
// longest v4 op name without truncation (the old 20-byte field cut it short).
TEST(ErrLog, CapacityAndLongOpNames) {
  obs::set_error_ring_capacity(4);
  obs::record_error_reply("peer", "BIND_CONN_TO_SESSION", 0xa000, 1);
  for (uint32_t i = 1; i < 4; ++i)
    obs::record_error_reply("peer", "SEQUENCE", 0xa000 + i, 1);
  auto out = obs::dump_error_replies();
  EXPECT_TRUE(out.find("proc=BIND_CONN_TO_SESSION") != std::string::npos);
  obs::record_error_reply("peer", "SEQUENCE", 0xa004, 1);  // evicts the oldest of 4
  out = obs::dump_error_replies();
  EXPECT_TRUE(out.find("xid=0xa000 ") == std::string::npos);
  EXPECT_TRUE(out.find("xid=0xa004") != std::string::npos);
  obs::set_error_ring_capacity(64);  // restore the default for later tests
}

// Sharded metric counters (plan doc 10 §2.6): concurrent bumps land in per-thread
// slots; load() must still sum to the exact total, including negative net values.
TEST(Metrics, ShardedCounterSumsAcrossThreads) {
  obs::ShardedCounter<uint64_t> c;
  obs::ShardedCounter<int64_t> net;
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&] {
      for (int i = 0; i < 10000; ++i) c.fetch_add(1, std::memory_order_relaxed);
      for (int i = 0; i < 100; ++i) net.fetch_add(3, std::memory_order_relaxed);
      for (int i = 0; i < 100; ++i) net.fetch_sub(2, std::memory_order_relaxed);
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(c.load(std::memory_order_relaxed), 80000u);
  EXPECT_EQ(net.load(std::memory_order_relaxed), 8 * 100);
}

// Latency histogram (plan doc 10 §3.2): observations land in the right bucket and the
// Prometheus rendering is cumulative with _sum/_count, so p99 is computable.
TEST(Metrics, HistogramBucketsAndExposition) {
  obs::LatencyHistogram h;
  h.observe_us(50);        // <= 100µs -> bucket 0
  h.observe_us(100);       // boundary is inclusive -> bucket 0
  h.observe_us(300);       // -> le=0.0005
  h.observe_us(9'000'000); // beyond the last bound -> +Inf only
  auto snap = h.snapshot();
  EXPECT_EQ(snap.count, 4u);
  EXPECT_EQ(snap.sum_us, 50u + 100 + 300 + 9'000'000);
  EXPECT_EQ(snap.buckets[0], 2u);
  EXPECT_EQ(snap.buckets[2], 1u);
  EXPECT_EQ(snap.buckets[obs::LatencyHistogram::kBuckets - 1], 1u);
  std::string out;
  obs::append_histogram(out, "t_seconds", "op=\"X\"", snap);
  EXPECT_TRUE(out.find("t_seconds_bucket{op=\"X\",le=\"0.0001\"} 2\n") != std::string::npos);
  EXPECT_TRUE(out.find("t_seconds_bucket{op=\"X\",le=\"0.0005\"} 3\n") != std::string::npos);
  EXPECT_TRUE(out.find("t_seconds_bucket{op=\"X\",le=\"5\"} 3\n") != std::string::npos);
  EXPECT_TRUE(out.find("t_seconds_bucket{op=\"X\",le=\"+Inf\"} 4\n") != std::string::npos);
  EXPECT_TRUE(out.find("t_seconds_count{op=\"X\"} 4\n") != std::string::npos);
}

// v4 per-op counters (plan doc 10 §3.1) surface in the exposition with op labels; the
// label text is stored at bump time so obs stays independent of the nfsv4 op table.
TEST(Metrics, V4OpSeriesInPrometheusText) {
  auto& m = obs::Metrics::instance();
  size_t idx = obs::v4_op_index(9);  // GETATTR
  m.v4_op_names[idx].store("GETATTR", std::memory_order_relaxed);
  m.v4_op_calls[idx].fetch_add(3, std::memory_order_relaxed);
  m.v4_op_errors[idx].fetch_add(1, std::memory_order_relaxed);
  m.v4_op_duration[idx].observe_us(120);
  m.v4_compound_duration.observe_us(500);
  auto text = obs::prometheus_text();
  EXPECT_TRUE(text.find("lightnfs_v4_op_calls_total{op=\"GETATTR\"} 3") !=
              std::string::npos);
  EXPECT_TRUE(text.find("lightnfs_v4_op_errors_total{op=\"GETATTR\"} 1") !=
              std::string::npos);
  EXPECT_TRUE(text.find("lightnfs_v4_op_duration_seconds_bucket{op=\"GETATTR\"") !=
              std::string::npos);
  EXPECT_TRUE(text.find("lightnfs_v4_compound_duration_seconds_count") !=
              std::string::npos);
  // Out-of-table opcodes collapse into slot 0 instead of indexing out of bounds.
  EXPECT_EQ(obs::v4_op_index(10044), 0u);
}

TEST(Metrics, SlowRequestThreshold) {
  EXPECT_EQ(obs::slow_request_threshold_us(), 0u);  // default off until configured
  obs::set_slow_request_threshold_us(250000);
  EXPECT_EQ(obs::slow_request_threshold_us(), 250000u);
  obs::set_slow_request_threshold_us(0);
}
