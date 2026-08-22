// Gateway byte-range lock table (design 07 §7.6): POSIX merge/split/upgrade, conflict
// reporting, owner release, range helpers.

#include "mini_test.hpp"

#include "state/lock_mgr.hpp"

using namespace lnfs;
using state::GatewayLockMgr;

namespace {

backend::LockOwnerId owner(uint8_t n) {
  backend::LockOwnerId id;
  id.len = 4;
  id.bytes[0] = static_cast<std::byte>(n);
  return id;
}

state::FileKey file(uint8_t n) {
  state::FileKey k;
  k.fsid = 1;
  k.oid.len = 2;
  k.oid.bytes[0] = static_cast<std::byte>(n);
  return k;
}

}  // namespace

TEST(LockMgr, PosixMergeSplitAndConflicts) {
  GatewayLockMgr mgr;
  auto f = file(1);
  auto a = owner(1), b = owner(2);
  // Shared locks from two owners overlap freely.
  EXPECT_FALSE(mgr.lock(f, a, {0, 100}, false).has_value());
  EXPECT_FALSE(mgr.lock(f, b, {50, 100}, false).has_value());
  // Exclusive over a shared region held by another owner conflicts; the report names b.
  auto c = mgr.lock(f, a, {60, 10}, true);
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(state::same_owner(c->owner, b));
  EXPECT_EQ(c->range.offset, 50u);
  EXPECT_EQ(c->range.length, 100u);
  EXPECT_FALSE(c->exclusive);
  // Upgrade inside a's own shared range beyond b's: carve + coalesce.
  mgr.unlock(f, b, {50, 100});
  EXPECT_FALSE(mgr.lock(f, a, {20, 30}, true).has_value());  // [0,20)R [20,50)W [50,100)R
  auto segs = mgr.segments(f);
  ASSERT_TRUE(segs.size() == 3);
  EXPECT_EQ(segs[0].start, 0u);
  EXPECT_EQ(segs[0].end, 20u);
  EXPECT_FALSE(segs[0].exclusive);
  EXPECT_EQ(segs[1].start, 20u);
  EXPECT_EQ(segs[1].end, 50u);
  EXPECT_TRUE(segs[1].exclusive);
  EXPECT_EQ(segs[2].start, 50u);
  EXPECT_EQ(segs[2].end, 100u);
  // Downgrade back to shared coalesces into one segment.
  EXPECT_FALSE(mgr.lock(f, a, {20, 30}, false).has_value());
  segs = mgr.segments(f);
  ASSERT_TRUE(segs.size() == 1);
  EXPECT_EQ(segs[0].end, 100u);
  // Split by unlock in the middle; "to EOF" lock; test() ignores the requester's own.
  mgr.unlock(f, a, {40, 20});
  segs = mgr.segments(f);
  ASSERT_TRUE(segs.size() == 2);
  EXPECT_FALSE(mgr.lock(f, a, {1000, UINT64_MAX}, true).has_value());
  EXPECT_FALSE(mgr.test(f, &a, {0, UINT64_MAX}, true).has_value());
  auto t = mgr.test(f, &b, {5000, 1}, false);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->range.length, UINT64_MAX);
  EXPECT_TRUE(t->exclusive);
  EXPECT_EQ(mgr.count_owner(f, a), 3u);
  EXPECT_EQ(mgr.total_segments(), 3u);
  EXPECT_EQ(mgr.release_owner(f, a), 3u);
  EXPECT_EQ(mgr.total_segments(), 0u);
  EXPECT_EQ(mgr.files_locked(), 0u);
  EXPECT_TRUE(mgr.segments(f).empty());
}

TEST(LockMgr, RangeHelpers) {
  EXPECT_EQ(GatewayLockMgr::range_end({10, 5}), 15u);
  EXPECT_EQ(GatewayLockMgr::range_end({10, UINT64_MAX}), UINT64_MAX);
  EXPECT_EQ(GatewayLockMgr::range_end({UINT64_MAX - 1, 5}), UINT64_MAX);
  auto r = GatewayLockMgr::to_range(3, UINT64_MAX);
  EXPECT_EQ(r.offset, 3u);
  EXPECT_EQ(r.length, UINT64_MAX);
}
