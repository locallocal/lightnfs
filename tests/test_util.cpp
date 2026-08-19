#include <string>

#include "mini_test.hpp"
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
