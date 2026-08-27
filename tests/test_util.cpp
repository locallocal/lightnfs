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
