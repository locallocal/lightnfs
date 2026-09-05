// Cluster role state machine (design 09 §9.5/§9.6, plan 10 C2) driven deterministically:
// the in-memory ClusterStore (mem_cluster_store.hpp) whose fence can be aged or taken by
// "another node", hooks that record their call order, and tick() instead of the timer
// thread.

#include "mini_test.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "mem_cluster_store.hpp"
#include "obs/metrics.hpp"
#include "server/cluster_controller.hpp"
#include "server/cluster_store.hpp"
#include "server/takeover_hook.hpp"

using namespace lnfs;
using namespace std::chrono_literals;

namespace {

using MemStore = test::MemClusterStore;

struct Recorder {
  std::vector<std::string> calls;
  std::vector<uint64_t> epochs;
  std::vector<server::TakeoverContext> takeovers;  // what the backends/hook were told
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
                [this](const server::TakeoverContext& ctx) -> Result<void> {
                  calls.push_back("takeover");
                  takeovers.push_back(ctx);
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

// Value of the first "name value" sample line in the exposition; -1 when absent.
long long metric(const std::string& name) {
  std::string text = obs::prometheus_text();
  size_t pos = text.find("\n" + name + " ");
  if (pos == std::string::npos) return -1;
  return std::stoll(text.substr(pos + name.size() + 2));
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
  // The takeover context (plan 10 D1): identity + no previous holder on a free fence.
  ASSERT_TRUE(rec.takeovers.size() == 1u);
  EXPECT_STREQ(rec.takeovers[0].identity.cluster_id, "cluster-ctrl-test");
  EXPECT_STREQ(rec.takeovers[0].identity.node, "gw1");
  EXPECT_EQ(rec.takeovers[0].identity.epoch, 5u);
  EXPECT_STREQ(rec.takeovers[0].prev_node, "");
  auto snap = ctl.snapshot();
  EXPECT_EQ(snap.epoch, 5u);
  EXPECT_EQ(snap.takeovers, 1u);
  ASSERT_TRUE(snap.fence.has_value());
  EXPECT_STREQ(snap.fence->node, "gw1");
  EXPECT_EQ(snap.fence->epoch, 5u);
  // The controller's metrics provider (plan 10 C4) reports the same takeover.
  EXPECT_EQ(metric("lightnfs_cluster_takeovers_total"), 1);
  EXPECT_EQ(metric("lightnfs_cluster_epoch"), 5);
  EXPECT_EQ(metric("lightnfs_cluster_activation_seconds_count"), 1);
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
  // gw2 replaced gw1's expired record: the hooks are told whom to evict.
  ASSERT_TRUE(rec2.takeovers.size() == 1u);
  EXPECT_STREQ(rec2.takeovers[0].prev_node, "gw1");
  EXPECT_STREQ(rec2.takeovers[0].identity.node, "gw2");
  EXPECT_EQ(rec2.takeovers[0].identity.epoch, 6u);
}

// plan 10 D1: the external takeover hook — spawned with the takeover in its
// environment, bounded by a timeout, its failures reported but never fatal.
TEST(ClusterController, TakeoverHookScriptEnvAndTimeout) {
  char tmpl[] = "/tmp/lnfs-hook-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  auto script = [&](const char* name, const char* body) {
    std::string path = dir + "/" + name;
    std::ofstream(path) << "#!/bin/sh\n" << body;
    ::chmod(path.c_str(), 0755);
    return path;
  };
  // Records its environment next to itself.
  std::string env_hook = script(
      "env.sh", "printf '%s|%s|%s|%s' \"$LNFS_CLUSTER_ID\" \"$LNFS_NODE\" \"$LNFS_EPOCH\" "
                "\"$LNFS_PREV_NODE\" > \"$0.out\"\n");
  std::string slow_hook = script("slow.sh", "sleep 30\n");
  std::string failing_hook = script("fail.sh", "exit 3\n");
  auto slurp = [](const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };

  // Through the controller: gw1 takes over gw-old's expired fence and the script sees
  // the identity, the minted epoch and the node it replaced.
  {
    MemStore store;
    store.epoch = 41;
    store.taken_by("gw-old", 41);
    store.age_out();
    Recorder rec;
    auto hooks = rec.hooks();
    hooks.backend_takeover = [&](const server::TakeoverContext& ctx) -> Result<void> {
      rec.calls.push_back("takeover");
      return server::run_takeover_hook(env_hook, ctx.identity, ctx.prev_node, 5s);
    };
    server::ClusterController ctl(config("gw1"), store, std::move(hooks));
    ctl.tick();
    EXPECT_TRUE(ctl.role() == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "takeover activate");
    EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|42|gw-old");
  }

  backend::ClusterIdentity id{"cluster-ctrl-test", "gw1", 7};
  // A stale LNFS_* variable in the daemon's own environment does not leak through.
  setenv("LNFS_PREV_NODE", "stale", 1);
  ASSERT_TRUE(server::run_takeover_hook(env_hook, id, "", 5s).has_value());
  EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|7|");
  unsetenv("LNFS_PREV_NODE");

  // Timeout: killed, reaped, ETIMEDOUT — and well before the script's own sleep.
  auto t0 = std::chrono::steady_clock::now();
  auto timed_out = server::run_takeover_hook(slow_hook, id, "gw-old", 200ms);
  auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(!timed_out.has_value());
  EXPECT_EQ(static_cast<int>(timed_out.error()), ETIMEDOUT);
  EXPECT_TRUE(elapsed < 5s);
  // Non-zero exit → EIO; a path that cannot be executed → the spawn errno.
  auto failed = server::run_takeover_hook(failing_hook, id, "", 5s);
  ASSERT_TRUE(!failed.has_value());
  EXPECT_EQ(static_cast<int>(failed.error()), EIO);
  auto missing = server::run_takeover_hook(dir + "/missing.sh", id, "", 5s);
  ASSERT_TRUE(!missing.has_value());
  EXPECT_EQ(static_cast<int>(missing.error()), ENOENT);

  // Through the controller again: a failing hook is only a warning — still Active.
  {
    MemStore store;
    Recorder rec;
    auto hooks = rec.hooks();
    hooks.backend_takeover = [&](const server::TakeoverContext& ctx) -> Result<void> {
      return server::run_takeover_hook(failing_hook, ctx.identity, ctx.prev_node, 5s);
    };
    server::ClusterController ctl(config("gw1"), store, std::move(hooks));
    ctl.tick();
    EXPECT_TRUE(ctl.role() == server::Role::kActive);
    EXPECT_EQ(ctl.snapshot().takeovers, 1u);
  }
  std::filesystem::remove_all(dir);
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
