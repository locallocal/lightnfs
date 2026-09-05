// Cluster role state machine (design 09 §9.5/§9.6, plan 10 C2) driven deterministically:
// an in-memory ClusterStore whose fence can be aged or taken by "another node", hooks
// that record their call order, and tick() instead of the timer thread.

#include "mini_test.hpp"

#include <cerrno>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "server/cluster_controller.hpp"
#include "server/cluster_store.hpp"

using namespace lnfs;
using namespace std::chrono_literals;

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct MemStore final : server::ClusterStore {
  uint64_t epoch = 0;
  std::optional<server::FenceRecord> fence;
  std::map<std::string, std::string> clients, digests;
  std::vector<std::string> log;
  Errno fail_renew = Errno::kOk;  // IO trouble injected into renew_fence
  Errno fail_read = Errno::kOk;

  Result<std::array<std::byte, 16>> load_or_create_key() override {
    return std::array<std::byte, 16>{std::byte{1}};
  }
  Result<uint64_t> read_epoch() override { return epoch; }
  Result<uint64_t> bump_epoch() override {
    log.push_back("epoch");
    return ++epoch;
  }
  Result<std::vector<std::string>> list_clients() override {
    std::vector<std::string> out;
    for (auto& [k, v] : clients) out.push_back(v);
    return out;
  }
  Result<void> put_client(std::string_view o) override {
    clients[std::string(o)] = std::string(o);
    return {};
  }
  Result<void> erase_client(std::string_view o) override {
    clients.erase(std::string(o));
    return {};
  }
  Result<std::optional<server::FenceRecord>> read_fence() override {
    if (fail_read != Errno::kOk) return Err(fail_read);
    return fence;
  }
  Result<server::FenceRecord> acquire_fence(std::string_view node, uint64_t e,
                                            std::chrono::milliseconds ttl,
                                            bool force) override {
    log.push_back(std::string("acquire") + (force ? "!" : ""));
    bool expired = fence && now_ms() > fence->expires_at_ms + server::kFenceSkewTolerance.count();
    if (fence && !force && fence->node != node && !expired) return Err(errno_from(EBUSY));
    fence = server::FenceRecord{std::string(node), e, now_ms() + ttl.count()};
    return *fence;
  }
  Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) override {
    log.push_back("renew");
    if (fail_renew != Errno::kOk) return Err(fail_renew);
    if (!fence || fence->node != node) return Err(errno_from(EPERM));
    fence->expires_at_ms = now_ms() + ttl.count();
    return {};
  }
  Result<void> release_fence(std::string_view node) override {
    log.push_back("release");
    if (!fence) return {};
    if (fence->node != node) return Err(errno_from(EPERM));
    fence.reset();
    return {};
  }
  Result<void> put_exports_digest(std::string_view n, std::string_view d) override {
    digests[std::string(n)] = std::string(d);
    return {};
  }
  Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() override {
    return std::vector<std::pair<std::string, std::string>>(digests.begin(), digests.end());
  }

  void age_out() { fence->expires_at_ms = now_ms() - 10000; }
  void taken_by(const std::string& other, uint64_t e) {
    fence = server::FenceRecord{other, e, now_ms() + 60000};
  }
};

struct Recorder {
  std::vector<std::string> calls;
  std::vector<uint64_t> epochs;
  Errno fail_activate = Errno::kOk;
  server::ClusterController::Hooks hooks() {
    return {.post = {},  // inline
            .activate =
                [this](uint64_t epoch) -> Result<void> {
                  calls.push_back("activate");
                  epochs.push_back(epoch);
                  if (fail_activate != Errno::kOk) return Err(fail_activate);
                  return {};
                },
            .deactivate = [this] { calls.push_back("deactivate"); },
            .backend_takeover =
                [this]() -> Result<void> {
                  calls.push_back("takeover");
                  return {};
                },
            .backend_reset = [this] { calls.push_back("reset"); }};
  }
};

core::ClusterConfig config(const std::string& node, const std::string& role = "auto",
                           const std::string& takeover = "auto") {
  core::ClusterConfig c;
  c.enabled = true;
  c.id = "cluster-ctrl-test";
  c.node = node;
  c.role = role;
  c.takeover = takeover;
  c.fence_lease_ms = 1000;
  return c;
}

std::string joined(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : " ") + s;
  return out;
}

}  // namespace

TEST(ClusterController, StandbyTakesOverAFreeOrExpiredFence) {
  MemStore store;
  store.epoch = 4;
  Recorder rec;
  server::ClusterController ctl(config("gw1"), store, rec.hooks());
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);

  // No fence at all: one tick takes over — fence, epoch, backend takeover, activate.
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kActive);
  EXPECT_STREQ(joined(store.log), "acquire epoch");
  EXPECT_STREQ(joined(rec.calls), "takeover activate");
  ASSERT_TRUE(rec.epochs.size() == 1u);
  EXPECT_EQ(rec.epochs[0], 5u);
  auto snap = ctl.snapshot();
  EXPECT_EQ(snap.epoch, 5u);
  EXPECT_EQ(snap.takeovers, 1u);
  ASSERT_TRUE(snap.fence.has_value());
  EXPECT_STREQ(snap.fence->node, "gw1");
  EXPECT_EQ(snap.fence->epoch, 5u);
  // Active ticks renew and change nothing else.
  ctl.tick();
  ctl.tick();
  EXPECT_STREQ(joined(store.log), "acquire epoch renew renew");
  EXPECT_TRUE(ctl.role() == server::Role::kActive);

  // A second node sees a live fence: nothing happens until it expires.
  MemStore& shared = store;
  Recorder rec2;
  server::ClusterController other(config("gw2"), shared, rec2.hooks());
  other.tick();
  EXPECT_TRUE(other.role() == server::Role::kStandby);
  EXPECT_TRUE(rec2.calls.empty());
  shared.age_out();  // gw1 died: no renewals
  other.tick();
  EXPECT_TRUE(other.role() == server::Role::kActive);
  EXPECT_STREQ(joined(rec2.calls), "takeover activate");
  EXPECT_EQ(rec2.epochs[0], 6u);
  EXPECT_STREQ(shared.fence->node, "gw2");
}

TEST(ClusterController, ActiveLosesFenceAndDrainsWithoutReleasingIt) {
  MemStore store;
  Recorder rec;
  server::ClusterController ctl(config("gw1"), store, rec.hooks());
  ctl.tick();
  ASSERT_TRUE(ctl.role() == server::Role::kActive);
  rec.calls.clear();
  store.log.clear();

  // Split brain / forced takeover elsewhere: the next renew is EPERM.
  store.taken_by("gw2", 9);
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);  // drained inline
  EXPECT_STREQ(joined(rec.calls), "deactivate reset");
  EXPECT_STREQ(joined(store.log), "renew");  // no release of gw2's record
  EXPECT_STREQ(store.fence->node, "gw2");
  auto snap = ctl.snapshot();
  EXPECT_EQ(snap.fence_lost, 1u);
  EXPECT_EQ(snap.epoch, 0u);
  ASSERT_TRUE(snap.fence.has_value());
  EXPECT_STREQ(snap.fence->node, "gw2");
  // Standby again: gw2's fence is live, so no automatic takeover.
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);

  // Fence unreachable: three consecutive renew failures drain too.
  store.fence.reset();
  ctl.tick();
  ASSERT_TRUE(ctl.role() == server::Role::kActive);
  rec.calls.clear();
  store.fail_renew = errno_from(EIO);
  ctl.tick();
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kActive);  // two strikes
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);
  EXPECT_STREQ(joined(rec.calls), "deactivate reset");
  EXPECT_EQ(ctl.snapshot().fence_lost, 2u);
  store.fail_renew = Errno::kOk;
}

TEST(ClusterController, ManualTakeoverPolicyAndForce) {
  MemStore store;
  store.taken_by("gw-old", 3);
  store.age_out();
  Recorder rec;
  server::ClusterController manual(config("gw1", "auto", "manual"), store, rec.hooks());
  manual.tick();
  manual.tick();
  EXPECT_TRUE(manual.role() == server::Role::kStandby);  // expired fence, still nothing
  EXPECT_TRUE(rec.calls.empty());
  // role = standby never takes over on its own either.
  Recorder rec_sb;
  server::ClusterController standby(config("gw3", "standby", "auto"), store, rec_sb.hooks());
  standby.tick();
  EXPECT_TRUE(standby.role() == server::Role::kStandby);
  EXPECT_TRUE(rec_sb.calls.empty());

  // The operator's takeover: an expired fence needs no force.
  ASSERT_TRUE(manual.request_takeover(false).has_value());
  EXPECT_TRUE(manual.role() == server::Role::kActive);
  EXPECT_EQ(rec.epochs[0], 1u);
  // Already active: a second request is EBUSY; standby request drains and releases.
  auto busy = manual.request_takeover(false);
  ASSERT_TRUE(!busy.has_value());
  EXPECT_EQ(static_cast<int>(busy.error()), EBUSY);
  store.log.clear();
  ASSERT_TRUE(manual.request_standby().has_value());
  EXPECT_TRUE(manual.role() == server::Role::kStandby);
  EXPECT_STREQ(joined(store.log), "release");
  EXPECT_FALSE(store.fence.has_value());
  auto not_active = manual.request_standby();
  ASSERT_TRUE(!not_active.has_value());
  EXPECT_EQ(static_cast<int>(not_active.error()), EINVAL);

  // A live fence held by another node: plain takeover is EBUSY, force wins.
  store.taken_by("gw2", 7);
  auto refused = manual.request_takeover(false);
  ASSERT_TRUE(!refused.has_value());
  EXPECT_EQ(static_cast<int>(refused.error()), EBUSY);
  EXPECT_TRUE(manual.role() == server::Role::kStandby);
  store.log.clear();
  ASSERT_TRUE(manual.request_takeover(true).has_value());
  EXPECT_TRUE(manual.role() == server::Role::kActive);
  EXPECT_STREQ(joined(store.log), "acquire! epoch");
  EXPECT_STREQ(store.fence->node, "gw1");
  EXPECT_EQ(manual.snapshot().takeovers, 2u);
}

TEST(ClusterController, ActivationFailureReleasesFenceAndKeepsEpochMonotonic) {
  MemStore store;
  store.epoch = 10;
  Recorder rec;
  rec.fail_activate = errno_from(EADDRINUSE);
  server::ClusterController ctl(config("gw1"), store, rec.hooks());
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);
  EXPECT_STREQ(joined(store.log), "acquire epoch release");
  EXPECT_STREQ(joined(rec.calls), "takeover activate");
  EXPECT_FALSE(store.fence.has_value());
  EXPECT_EQ(store.epoch, 11u);  // consumed: monotonic is all that matters
  auto snap = ctl.snapshot();
  EXPECT_EQ(snap.activation_failures, 1u);
  EXPECT_EQ(snap.takeovers, 0u);
  // Fixed: the next tick tries again with a fresh epoch.
  rec.fail_activate = Errno::kOk;
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kActive);
  EXPECT_EQ(rec.epochs.back(), 12u);
}

TEST(ClusterController, PostedActivationRenewsMeanwhileAndTimerThreadRuns) {
  // The data-plane work runs "elsewhere": queued, not inline.  The controller is
  // Activating meanwhile and keeps renewing the fence.
  MemStore store;
  Recorder rec;
  std::vector<std::function<void()>> queue;
  auto hooks = rec.hooks();
  hooks.post = [&queue](std::function<void()> fn) { queue.push_back(std::move(fn)); };
  server::ClusterController ctl(config("gw1"), store, std::move(hooks));
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kActivating);
  EXPECT_TRUE(rec.calls.empty());
  EXPECT_EQ(queue.size(), 1u);
  ctl.tick();
  EXPECT_STREQ(joined(store.log), "acquire epoch renew");
  // Run the queued work: Active.
  queue.front()();
  queue.clear();
  EXPECT_TRUE(ctl.role() == server::Role::kActive);
  EXPECT_STREQ(joined(rec.calls), "takeover activate");
  // A lost fence while Activating would also be handled once the work lands: the
  // drain is queued, the role is Draining, and Standby only after it ran.
  store.taken_by("gw2", 5);
  ctl.tick();
  EXPECT_TRUE(ctl.role() == server::Role::kDraining);
  EXPECT_EQ(queue.size(), 1u);
  ctl.tick();  // Draining ticks do nothing
  queue.front()();
  EXPECT_TRUE(ctl.role() == server::Role::kStandby);

  // The timer thread: with a 1 s lease and a free fence it takes over within a tick.
  MemStore store2;
  Recorder rec2;
  server::ClusterController timed(config("gw9"), store2, rec2.hooks());
  timed.start();
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (timed.role() != server::Role::kActive && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(timed.role() == server::Role::kActive);
  timed.stop();
  EXPECT_TRUE(timed.role() == server::Role::kActive);  // stop() leaves the role alone
  EXPECT_STREQ(store2.fence->node, "gw9");
}
