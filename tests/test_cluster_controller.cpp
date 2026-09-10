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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mem_cluster_store.hpp"
#include "obs/metrics.hpp"
#include "server/catalog_applier.hpp"
#include "server/catalog_boot.hpp"
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
    // what the backends/hook were told
    std::vector<server::TakeoverContext> takeovers;
    Errno fail_activate = Errno::kOk;
    server::ClusterController::Hooks hooks() {
        // inline
        return {.post = {},
                .activate = [this](uint64_t epoch) -> Result<void> {
                    calls.push_back("activate");
                    epochs.push_back(epoch);
                    if (fail_activate != Errno::kOk) return Err(fail_activate);
                    return {};
                },
                .deactivate = [this] { calls.push_back("deactivate"); },
                .backend_takeover = [this](const server::TakeoverContext& ctx) -> Result<void> {
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
    // gw1 died: no renewals
    shared.age_out();
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
    std::string env_hook = script("env.sh",
                                  "printf '%s|%s|%s|%s|%s|%s' \"$LNFS_CLUSTER_ID\" \"$LNFS_NODE\" \"$LNFS_EPOCH\" "
                                  "\"$LNFS_PREV_NODE\" \"$LNFS_FSID\" \"$LNFS_REASON\" > \"$0.out\"\n");
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
        EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|42|gw-old||takeover");
    }

    backend::ClusterIdentity id{"cluster-ctrl-test", "gw1", 7};
    // A stale LNFS_* variable in the daemon's own environment does not leak through.
    setenv("LNFS_PREV_NODE", "stale", 1);
    setenv("LNFS_FSID", "stale", 1);
    setenv("LNFS_REASON", "stale", 1);
    ASSERT_TRUE(server::run_takeover_hook(env_hook, id, "", 5s).has_value());
    EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|7|||takeover");
    unsetenv("LNFS_PREV_NODE");
    unsetenv("LNFS_FSID");
    unsetenv("LNFS_REASON");
    // Scoped to one export (active-active, plan 12 C2): LNFS_FSID names it; a planned
    // migration says so (plan 12 D1).
    ASSERT_TRUE(server::run_takeover_hook(env_hook, id, "gw-old", 5s, 12).has_value());
    EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|7|gw-old|12|takeover");
    ASSERT_TRUE(server::run_takeover_hook(env_hook, id, "gw-old", 5s, 12, "migrate").has_value());
    EXPECT_STREQ(slurp(env_hook + ".out"), "cluster-ctrl-test|gw1|7|gw-old|12|migrate");

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
    // drained inline
    EXPECT_TRUE(ctl.role() == server::Role::kStandby);
    EXPECT_STREQ(joined(rec.calls), "deactivate reset");
    // no release of gw2's record
    EXPECT_STREQ(joined(store.log), "renew");
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
    // two strikes
    EXPECT_TRUE(ctl.role() == server::Role::kActive);
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
    // expired fence, still nothing
    EXPECT_TRUE(manual.role() == server::Role::kStandby);
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
    // consumed: monotonic is all that matters
    EXPECT_EQ(store.epoch, 11u);
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
    // Draining ticks do nothing
    ctl.tick();
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
    // stop() leaves the role alone
    EXPECT_TRUE(timed.role() == server::Role::kActive);
    EXPECT_STREQ(store2.fence->node, "gw9");
}

// ---- active-active: FsClusterController (design 10 §10.3/§10.14, plan 12 C1) ----------
//
// Same shape as above: MemClusterStore, inline hooks that record their calls per
// fsid, tick() by hand.  The view the controller publishes is what the v4 engine
// reads (plan 12 B2/B3), so its roles are checked alongside the controller's own.

#include "backend/memory/memory.hpp"
#include "core/fs_owner_view.hpp"

namespace {

struct FsRecorder {
    // "takeover:F" / "activate:F" / "deactivate:F"
    std::vector<std::string> calls;
    std::vector<server::TakeoverContext> takeovers;
    Errno fail_activate = Errno::kOk;
    server::FsClusterController::Hooks hooks() {
        return {.post = {},
                .activate_fs = [this](uint32_t fsid, uint64_t) -> Result<void> {
                    calls.push_back("activate:" + std::to_string(fsid));
                    if (fail_activate != Errno::kOk) return Err(fail_activate);
                    return {};
                },
                .deactivate_fs = [this](uint32_t fsid) { calls.push_back("deactivate:" + std::to_string(fsid)); },
                .backend_takeover = [this](uint32_t fsid, const server::TakeoverContext& ctx) -> Result<void> {
                    calls.push_back("takeover:" + std::to_string(fsid));
                    takeovers.push_back(ctx);
                    return {};
                }};
    }
};

// Three exports (fsid 1, 2, 3) with the given owner lists.
struct Exports {
    core::ExportTable table;
    explicit Exports(std::vector<std::vector<std::string>> nodes) {
        for (uint32_t fsid = 1; fsid <= 3; ++fsid) {
            core::ExportConfig cfg;
            cfg.path = "/export/" + std::to_string(fsid);
            cfg.fsid = fsid;
            cfg.clients = {"127.0.0.0/8"};
            cfg.nodes = nodes[fsid - 1];
            auto ok = table.add(cfg, std::make_unique<backend::MemoryBackend>(fsid));
            ASSERT_TRUE(ok.has_value());
        }
    }
};

core::ClusterConfig aa_config(const std::string& node, const std::string& takeover = "auto") {
    core::ClusterConfig c = config(node, "auto", takeover);
    c.mode = "active-active";
    // gwN → 10.0.0.N
    c.node_address = "10.0.0." + node.substr(2) + ":2049";
    return c;
}

server::Role fs_role(const server::FsClusterController& ctl, uint32_t fsid) {
    return ctl.role_of(fsid);
}

const core::FsOwner& view_of(const core::FsOwnerView& view, uint32_t fsid) {
    static core::FsOwner none;
    auto snap = view.snapshot();
    auto it = snap->find(fsid);
    return it == snap->end() ? none : it->second;
}

size_t count(const std::vector<std::string>& log, const std::string& entry) {
    return static_cast<size_t>(std::count(log.begin(), log.end(), entry));
}

}  // namespace

// The single-gateway degeneration gate (design 10 §10.13): one node listed for every
// export takes them all, under one fence record, and the view says Active for each.
TEST(FsClusterController, SingleNodeOwnsEveryFsid) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw1"}, {"gw1"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    // Before the first tick nothing is served: Unowned, never silently Active.
    for (uint32_t f = 1; f <= 3; ++f) {
        EXPECT_TRUE(fs_role(ctl, f) == server::Role::kStandby);
        EXPECT_TRUE(view_of(view, f).role == core::FsRole::kUnowned);
    }
    for (int i = 0; i < 3; ++i) ctl.tick();
    for (uint32_t f = 1; f <= 3; ++f) {
        EXPECT_TRUE(fs_role(ctl, f) == server::Role::kActive);
        const auto& o = view_of(view, f);
        EXPECT_TRUE(o.role == core::FsRole::kActive);
        EXPECT_STREQ(o.node, "gw1");
        EXPECT_STREQ(o.address, "10.0.0.1:2049");
        EXPECT_EQ(o.fs_epoch, 1u);
        // Hook order per export: storage eviction, then the grace window.
        EXPECT_EQ(count(rec.calls, "takeover:" + std::to_string(f)), 1u);
        EXPECT_EQ(count(rec.calls, "activate:" + std::to_string(f)), 1u);
        // The owner record names us with our address and the minted fs epoch.
        ASSERT_TRUE(store.owners.contains(f));
        EXPECT_STREQ(store.owners[f].node, "gw1");
        EXPECT_STREQ(store.owners[f].address, "10.0.0.1:2049");
        EXPECT_EQ(store.owners[f].fs_epoch, 1u);
        EXPECT_EQ(store.fs_epochs[f], 1u);
    }
    // One fence record lists all three.
    ASSERT_TRUE(store.fences.size() == 1u);
    ASSERT_TRUE(store.fences["gw1"].holds.size() == 3u);
    EXPECT_EQ(store.fences["gw1"].holds[0].fsid, 1u);
    EXPECT_EQ(store.fences["gw1"].holds[2].fsid, 3u);
    // A free fence: no previous holder to evict.
    ASSERT_TRUE(rec.takeovers.size() == 3u);
    EXPECT_STREQ(rec.takeovers[0].prev_node, "");
    EXPECT_STREQ(rec.takeovers[0].identity.node, "gw1");
    EXPECT_EQ(rec.takeovers[0].identity.epoch, 1u);
    auto snap = ctl.snapshot();
    ASSERT_TRUE(snap.size() == 3u);
    EXPECT_EQ(snap[1].fsid, 2u);
    EXPECT_EQ(snap[1].takeovers, 1u);
    ASSERT_TRUE(snap[1].fence.has_value());
    EXPECT_STREQ(snap[1].fence->node, "gw1");
    ASSERT_TRUE(snap[1].owner.has_value());
    EXPECT_STREQ(snap[1].owner->node, "gw1");
}

// The batched lease (design 10 §10.3): holding N exports costs one store write per
// tick, and a steady tick writes nothing else.
TEST(FsClusterController, RenewIsOneWritePerTick) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw1"}, {"gw1"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    ctl.tick();
    ASSERT_TRUE(fs_role(ctl, 3) == server::Role::kActive);
    // The first tick: one renew (the heartbeat, before anything is held), then per
    // export fence + epoch + owner.
    EXPECT_EQ(count(store.log, "renew_fences"), 1u);
    EXPECT_EQ(count(store.log, "acquire_fs:1"), 1u);
    EXPECT_EQ(count(store.log, "put_owner:3=gw1"), 1u);
    store.log.clear();
    rec.calls.clear();
    for (int i = 0; i < 5; ++i) ctl.tick();
    EXPECT_EQ(store.log.size(), 5u);
    EXPECT_EQ(count(store.log, "renew_fences"), 5u);
    EXPECT_TRUE(rec.calls.empty());
    int64_t expires = store.fences["gw1"].expires_at_ms;
    // 3 × 1 s lease, just renewed
    EXPECT_TRUE(expires > test::wall_now_ms() + 2500);
}

// One export taken elsewhere (forced takeover or split brain) drains that export
// alone: its state is dropped, its fence is not released (it is theirs now), and the
// other two stay in service.
TEST(FsClusterController, ActiveLosesOneFsidDrainsOnlyThat) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw1", "gw2"}, {"gw1", "gw2"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    ctl.tick();
    ASSERT_TRUE(fs_role(ctl, 2) == server::Role::kActive);
    rec.calls.clear();
    store.log.clear();

    // gw2 now lists F2 (its record live) at fs epoch 7, and has written the owner record.
    store.fs_taken_by(2, "gw2", 7);
    store.fs_epochs[2] = 7;
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    (void)store.put_owner(2, {"gw2", "10.0.0.2:2049", 7});
    store.log.clear();
    ctl.tick();
    // drained inline
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    EXPECT_TRUE(fs_role(ctl, 3) == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "deactivate:2");
    // no release of gw2's hold
    EXPECT_STREQ(joined(store.log), "renew_fences");
    ASSERT_TRUE(store.fences["gw2"].holds.size() == 1u);
    ASSERT_TRUE(store.fences["gw1"].holds.size() == 2u);
    // The view refers F2 to gw2 (owner record: address + fs epoch), serves F1/F3.
    const auto& two = view_of(view, 2);
    EXPECT_TRUE(two.role == core::FsRole::kRemote);
    EXPECT_STREQ(two.node, "gw2");
    EXPECT_STREQ(two.address, "10.0.0.2:2049");
    EXPECT_EQ(two.fs_epoch, 7u);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kActive);
    EXPECT_TRUE(view_of(view, 3).role == core::FsRole::kActive);
    auto snap = ctl.snapshot();
    EXPECT_EQ(snap[1].fence_lost, 1u);
    EXPECT_EQ(snap[1].fs_epoch, 0u);
    EXPECT_EQ(snap[0].fence_lost, 0u);
    // gw2 holds F2 live and stands ahead of nobody: F2 stays Remote on further ticks.
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kRemote);

    // gw2 dies: its record ages out — F2 is Unowned in the view, then ours again (we
    // are first in its list) with a fresh fs epoch and gw2 named as the node to evict.
    store.age_out_node("gw2");
    rec.calls.clear();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "takeover:2 activate:2");
    EXPECT_STREQ(rec.takeovers.back().prev_node, "gw2");
    EXPECT_EQ(rec.takeovers.back().identity.epoch, 8u);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kActive);
    EXPECT_EQ(view_of(view, 2).fs_epoch, 8u);
    EXPECT_TRUE(store.fences["gw2"].holds.empty());

    // The fence store unreachable: three failed renews drain everything we hold, without
    // releasing (the records may still be intact).
    rec.calls.clear();
    store.fail_renew = errno_from(EIO);
    ctl.tick();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    ctl.tick();
    for (uint32_t f = 1; f <= 3; ++f) EXPECT_TRUE(fs_role(ctl, f) == server::Role::kStandby);
    EXPECT_EQ(count(rec.calls, "deactivate:1"), 1u);
    EXPECT_EQ(count(rec.calls, "deactivate:3"), 1u);
    EXPECT_EQ(ctl.snapshot()[0].fence_lost, 1u);
    // not released
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 3u);
    // Still unreachable: nothing is retaken (a fence we cannot renew is not worth
    // taking); once the store answers again our own live record makes them ours.
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);
    store.fail_renew = Errno::kOk;
    ctl.tick();
    for (uint32_t f = 1; f <= 3; ++f) EXPECT_TRUE(fs_role(ctl, f) == server::Role::kActive);
    // our own record: nobody to evict
    EXPECT_STREQ(rec.takeovers.back().prev_node, "");
}

// Remote and unowned exports as the view reports them, and the node-order policy:
// a node yields to a live predecessor, takes over once it is gone, and never takes an
// export it is not listed for.
TEST(FsClusterController, RemoteUnownedAndNodeOrder) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw2", "gw1"}, {"gw3"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    // gw2 is alive (heartbeat) but holds nothing yet.
    (void)store.renew_fences("gw2", 60s);
    ctl.tick();
    // first in line
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    // gw2's turn: yielded
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kStandby);
    // not ours to take
    EXPECT_TRUE(fs_role(ctl, 3) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kUnowned);
    EXPECT_TRUE(view_of(view, 3).role == core::FsRole::kUnowned);
    EXPECT_STREQ(view_of(view, 2).address, "");
    // gw2 takes F2 (fence only, owner record not yet written): Remote, address from the
    // node registry and the fs epoch from the fence.
    store.fs_taken_by(2, "gw2", 3);
    store.fs_epochs[2] = 3;
    ctl.tick();
    const auto& two = view_of(view, 2);
    EXPECT_TRUE(two.role == core::FsRole::kRemote);
    EXPECT_STREQ(two.node, "gw2");
    EXPECT_STREQ(two.address, "10.0.0.2:2049");
    EXPECT_EQ(two.fs_epoch, 3u);
    // gw2 dies: F2 is second-in-line ours; F3 stays Unowned forever (gw3 only), naming
    // nobody.
    store.age_out_node("gw2");
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers.back().prev_node, "gw2");
    EXPECT_EQ(view_of(view, 2).fs_epoch, 4u);
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 3) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 3).role == core::FsRole::kUnowned);
    EXPECT_STREQ(view_of(view, 3).node, "");

    // takeover = manual: only ever the view.
    MemStore store2;
    core::FsOwnerView view2;
    FsRecorder rec2;
    server::FsClusterController manual(aa_config("gw1", "manual"), exports.table, store2, view2, rec2.hooks());
    manual.tick();
    manual.tick();
    for (uint32_t f = 1; f <= 3; ++f) EXPECT_TRUE(fs_role(manual, f) == server::Role::kStandby);
    EXPECT_TRUE(rec2.calls.empty());
    EXPECT_EQ(count(store2.log, "renew_fences"), 2u);
}

// Operator requests (the ctl surface arrives with plan 12 C4): takeover of a Remote
// export (--force over a live foreign fence), release of an Active one — which is not
// retaken automatically until another node has held it — and the error codes.
TEST(FsClusterController, OperatorTakeoverAndRelease) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw2"}, {"gw1"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    store.fs_taken_by(2, "gw2", 5);
    store.fs_epochs[2] = 5;
    ctl.tick();
    ASSERT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    ASSERT_TRUE(fs_role(ctl, 2) == server::Role::kStandby);

    // A live foreign fence: plain takeover is EBUSY, force wins and evicts gw2.
    auto busy = ctl.request_takeover(2, false);
    ASSERT_TRUE(!busy.has_value());
    EXPECT_EQ(static_cast<int>(busy.error()), EBUSY);
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kStandby);
    store.log.clear();
    ASSERT_TRUE(ctl.request_takeover(2, true).has_value());
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kActive);
    EXPECT_STREQ(joined(store.log), "acquire_fs:2! fs_epoch:2 put_owner:2=gw1");
    EXPECT_STREQ(rec.takeovers.back().prev_node, "gw2");
    EXPECT_EQ(view_of(view, 2).fs_epoch, 6u);
    EXPECT_TRUE(store.fences["gw2"].holds.empty());
    // Already ours / unknown fsid.
    auto again = ctl.request_takeover(2, false);
    ASSERT_TRUE(!again.has_value());
    EXPECT_EQ(static_cast<int>(again.error()), EBUSY);
    auto unknown = ctl.request_takeover(9, false);
    ASSERT_TRUE(!unknown.has_value());
    EXPECT_EQ(static_cast<int>(unknown.error()), EINVAL);

    // Release F1: drained, fence hold dropped, and further ticks leave it alone even
    // though we are first in its list.
    rec.calls.clear();
    store.log.clear();
    ASSERT_TRUE(ctl.request_release(1).has_value());
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_STREQ(joined(rec.calls), "deactivate:1");
    EXPECT_STREQ(joined(store.log), "release_fs:1");
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 2u);
    ctl.tick();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);
    auto not_active = ctl.request_release(1);
    ASSERT_TRUE(!not_active.has_value());
    EXPECT_EQ(static_cast<int>(not_active.error()), EINVAL);
    // An explicit takeover request lifts the hold-off; so does another node holding it.
    ASSERT_TRUE(ctl.request_takeover(1, false).has_value());
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    ASSERT_TRUE(ctl.request_release(1).has_value());
    store.fs_taken_by(1, "gw2", 9);
    ctl.tick();
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kRemote);
    store.age_out_node("gw2");
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
}

// Posted (not inline) data-plane work: Activating shows as Unowned in the view and
// the batched renew keeps the fence meanwhile; a failed activation releases the
// fence and counts; shutdown drains every Active export inline and leaves the
// (empty) record as the heartbeat.
TEST(FsClusterController, PostedActivationFailureAndShutdown) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw1"}, {"gw2"}});
    core::FsOwnerView view;
    FsRecorder rec;
    std::vector<std::function<void()>> queue;
    auto hooks = rec.hooks();
    hooks.post = [&queue](std::function<void()> fn) { queue.push_back(std::move(fn)); };
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, std::move(hooks));
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActivating);
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kActivating);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_EQ(queue.size(), 2u);
    ASSERT_TRUE(store.fences["gw1"].holds.size() == 2u);
    // still Activating: renewed, not re-taken, not drained
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActivating);
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(count(store.log, "acquire_fs:1"), 1u);
    // F1's work fails, F2's succeeds.
    rec.fail_activate = errno_from(EIO);
    queue[0]();
    rec.fail_activate = Errno::kOk;
    queue[1]();
    queue.clear();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(ctl, 2) == server::Role::kActive);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kActive);
    EXPECT_EQ(ctl.snapshot()[0].activation_failures, 1u);
    EXPECT_EQ(ctl.snapshot()[0].takeovers, 0u);
    EXPECT_EQ(ctl.snapshot()[1].takeovers, 1u);
    ASSERT_TRUE(store.fences["gw1"].holds.size() == 1u);
    EXPECT_EQ(store.fences["gw1"].holds[0].fsid, 2u);
    // The next tick retries F1 with a fresh fs epoch.
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActivating);
    queue.front()();
    queue.clear();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    EXPECT_EQ(view_of(view, 1).fs_epoch, 2u);

    // Exit: both drained inline (no post), fences released, record kept, view Unowned.
    rec.calls.clear();
    store.log.clear();
    ctl.shutdown();
    EXPECT_TRUE(queue.empty());
    EXPECT_STREQ(joined(rec.calls), "deactivate:1 deactivate:2");
    EXPECT_STREQ(joined(store.log), "release_fs:1 release_fs:2");
    EXPECT_TRUE(store.fences.contains("gw1"));
    EXPECT_TRUE(store.fences["gw1"].holds.empty());
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kUnowned);
    // idempotent
    ctl.shutdown();

    // The timer thread: with a 1 s lease it takes its exports over within a tick.
    MemStore store2;
    core::FsOwnerView view2;
    FsRecorder rec2;
    server::FsClusterController timed(aa_config("gw1"), exports.table, store2, view2, rec2.hooks());
    timed.start();
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (fs_role(timed, 2) != server::Role::kActive && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    timed.stop();
    EXPECT_TRUE(fs_role(timed, 1) == server::Role::kActive);
    EXPECT_TRUE(fs_role(timed, 2) == server::Role::kActive);
    EXPECT_TRUE(fs_role(timed, 3) == server::Role::kStandby);
}

// ---- plan 12 C2: automatic per-fsid takeover by node order -----------------------------

// F1's owner list is [gw1, gw2, gw3].  gw1's record has lapsed; while gw2's heartbeat is
// live gw3 yields (gw2 will take it); once gw2 has lapsed too gw3 takes it — storage
// eviction, grace, owner record, in that order, naming gw1 as the node evicted.
TEST(FsClusterController, TakeoverFollowsNodeOrder) {
    MemStore store;
    Exports exports({{"gw1", "gw2", "gw3"}, {"gw1", "gw2", "gw3"}, {"gw1", "gw2", "gw3"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController gw3(aa_config("gw3"), exports.table, store, view, rec.hooks());
    store.fs_taken_by(1, "gw1", 4);
    store.fs_epochs[1] = 4;
    (void)store.put_owner(1, {"gw1", "10.0.0.1:2049", 4});
    // alive, holds nothing
    (void)store.renew_fences("gw2", 60s);
    // dead one second ago
    store.age_out_node("gw1", 1000);
    gw3.tick();
    gw3.tick();
    EXPECT_TRUE(fs_role(gw3, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);
    // who lapsed
    EXPECT_STREQ(view_of(view, 1).node, "gw1");
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_EQ(count(store.log, "acquire_fs:1"), 0u);
    // gw2 lapses as well: gw3 is next in line.
    store.age_out_node("gw2", 1000);
    store.log.clear();
    gw3.tick();
    EXPECT_TRUE(fs_role(gw3, 1) == server::Role::kActive);
    EXPECT_TRUE(joined(rec.calls).starts_with("takeover:1 activate:1"));
    EXPECT_TRUE(joined(store.log).starts_with("renew_fences acquire_fs:1 fs_epoch:1 put_owner:1=gw3"));
    // F2/F3 on the same tick, see below
    ASSERT_TRUE(rec.takeovers.size() == 3u);
    EXPECT_STREQ(rec.takeovers[0].prev_node, "gw1");
    EXPECT_EQ(rec.takeovers[0].identity.epoch, 5u);
    EXPECT_STREQ(rec.takeovers[0].identity.node, "gw3");
    EXPECT_STREQ(store.owners[1].node, "gw3");
    EXPECT_EQ(store.owners[1].fs_epoch, 5u);
    EXPECT_TRUE(store.fences["gw1"].holds.empty());
    const auto& one = view_of(view, 1);
    EXPECT_TRUE(one.role == core::FsRole::kActive);
    EXPECT_EQ(one.fs_epoch, 5u);
    // F2/F3 were never held: same order, gw2 dead → gw3 takes them on that tick too.
    EXPECT_TRUE(fs_role(gw3, 2) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers[1].prev_node, "");
}

// A dead gateway's exports spread over its successors (design 10 §10.8): gw1 held F1
// (list [gw1, gw2, gw3]) and F2 (list [gw1, gw3, gw2]); after it lapses gw2 takes F1
// and gw3 takes F2, each yielding to the other on the export where it is next.
TEST(FsClusterController, DeadNodeSpreadsAcrossSuccessors) {
    MemStore store;
    Exports exports({{"gw1", "gw2", "gw3"}, {"gw1", "gw3", "gw2"}, {"gw1"}});
    core::FsOwnerView view2, view3;
    FsRecorder rec2, rec3;
    server::FsClusterController gw2(aa_config("gw2"), exports.table, store, view2, rec2.hooks());
    server::FsClusterController gw3(aa_config("gw3"), exports.table, store, view3, rec3.hooks());
    for (uint32_t f : {1u, 2u, 3u}) {
        store.fs_taken_by(f, "gw1", 2);
        store.fs_epochs[f] = 2;
    }
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    // Everyone alive: both successors see three Remote exports and take nothing.
    gw2.tick();
    gw3.tick();
    for (uint32_t f : {1u, 2u, 3u}) {
        EXPECT_TRUE(view_of(view2, f).role == core::FsRole::kRemote);
        EXPECT_TRUE(view_of(view3, f).role == core::FsRole::kRemote);
        EXPECT_STREQ(view_of(view3, f).node, "gw1");
    }
    EXPECT_TRUE(store.fences.contains("gw2") && store.fences.contains("gw3"));
    // gw1 dies.  Alternating ticks: each takes the export it is next for and leaves the
    // other's alone (its heartbeat is live), F3 stays with nobody (gw1 only).
    store.age_out_node("gw1", 1000);
    gw2.tick();
    gw3.tick();
    gw2.tick();
    gw3.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kActive);
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw3, 1) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw3, 2) == server::Role::kActive);
    EXPECT_TRUE(fs_role(gw2, 3) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw3, 3) == server::Role::kStandby);
    EXPECT_STREQ(joined(rec2.calls), "takeover:1 activate:1");
    EXPECT_STREQ(joined(rec3.calls), "takeover:2 activate:2");
    EXPECT_STREQ(rec2.takeovers[0].prev_node, "gw1");
    EXPECT_STREQ(rec3.takeovers[0].prev_node, "gw1");
    EXPECT_STREQ(store.owners[1].node, "gw2");
    EXPECT_STREQ(store.owners[2].node, "gw3");
    EXPECT_EQ(store.owners[1].fs_epoch, 3u);
    EXPECT_EQ(store.owners[2].fs_epoch, 3u);
    ASSERT_TRUE(store.fences["gw2"].holds.size() == 1u);
    ASSERT_TRUE(store.fences["gw3"].holds.size() == 1u);
    EXPECT_EQ(store.fences["gw2"].holds[0].fsid, 1u);
    EXPECT_EQ(store.fences["gw3"].holds[0].fsid, 2u);
    // F3, lapsed
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 1u);
    // Each view refers the other's export to its new owner, with its address.
    EXPECT_TRUE(view_of(view2, 2).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view2, 2).node, "gw3");
    EXPECT_STREQ(view_of(view2, 2).address, "10.0.0.3:2049");
    EXPECT_EQ(view_of(view2, 2).fs_epoch, 3u);
    EXPECT_TRUE(view_of(view3, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view3, 1).node, "gw2");
    EXPECT_TRUE(view_of(view2, 3).role == core::FsRole::kUnowned);
    EXPECT_TRUE(view_of(view3, 3).role == core::FsRole::kUnowned);
}

// An export nobody takes: its live predecessor sits idle (takeover = manual, say) —
// after 2 × ttl unowned the successor stops yielding.  Both clocks: a lapsed record
// (unowned since its expiry) and no record at all (since first seen free).
TEST(FsClusterController, StuckUnownedFsidSkipsIdlePredecessor) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw1", "gw2"}, {"gw1", "gw2"}});
    core::FsOwnerView view;
    FsRecorder rec;
    core::ClusterConfig cfg = aa_config("gw2");
    // ttl 300 ms, stuck after 600 ms
    cfg.fence_lease_ms = 100;
    server::FsClusterController gw2(cfg, exports.table, store, view, rec.hooks());
    // alive, idle
    (void)store.renew_fences("gw1", 60s);
    // F1: held by gw0 (a node since removed from the list) whose record lapsed 700 ms
    // ago, and gw1 — live, first in line — has not taken it: already past the wait,
    // gw2 takes it on its first tick, evicting gw0.
    store.fs_taken_by(1, "gw0", 6);
    store.fs_epochs[1] = 6;
    store.age_out_node("gw0", 700);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers.back().prev_node, "gw0");
    // F2/F3: never held.  gw1 is live and first: gw2 yields...
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kUnowned);
    std::this_thread::sleep_for(350ms);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    // ... until they have been unowned for 2 × ttl.
    std::this_thread::sleep_for(350ms);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kActive);
    EXPECT_TRUE(fs_role(gw2, 3) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers.back().prev_node, "");
    auto snap = gw2.snapshot();
    EXPECT_EQ(snap[0].takeovers + snap[1].takeovers + snap[2].takeovers, 3u);
    // The stuck clock resets once someone holds the export: gw1 takes F2 back by force
    // (fence and owner record), then releases it — gw2 waits the full 2 × ttl again
    // before overriding gw1.
    store.fs_taken_by(2, "gw1", 9);
    (void)store.put_owner(2, {"gw1", "10.0.0.1:2049", 9});
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 2).role == core::FsRole::kRemote);
    ASSERT_TRUE(store.release_fs_fence(2, "gw1").has_value());
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    std::this_thread::sleep_for(700ms);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kActive);
}

// Gateways starting together (design 10 §10.8): a predecessor that has not written
// any record yet is not dead — for one ttl after our own start we leave its exports
// alone; once it has heartbeated we keep yielding, and only silence past that window
// counts as dead.  An expired record is dead at once.
TEST(FsClusterController, UnheardPredecessorGetsOneTtlAtStartup) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw3", "gw2"}, {"gw2"}});
    core::FsOwnerView view;
    FsRecorder rec;
    core::ClusterConfig cfg = aa_config("gw2");
    // ttl 300 ms
    cfg.fence_lease_ms = 100;
    server::FsClusterController gw2(cfg, exports.table, store, view, rec.hooks());
    // gw1 (F1) and gw3 (F2) have no record; F3 is ours outright.
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw2, 3) == server::Role::kActive);
    // gw1 heartbeats inside the window: F1 stays its.  gw3 never shows up.
    (void)store.renew_fences("gw1", 60s);
    std::this_thread::sleep_for(350ms);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kStandby);
    EXPECT_TRUE(fs_role(gw2, 2) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers.back().prev_node, "");
    // A fresh controller and a predecessor whose record has lapsed: no waiting.
    MemStore store2;
    core::FsOwnerView view2;
    FsRecorder rec2;
    server::FsClusterController late(cfg, exports.table, store2, view2, rec2.hooks());
    (void)store2.renew_fences("gw1", 60s);
    store2.age_out_node("gw1", 1000);
    late.tick();
    EXPECT_TRUE(fs_role(late, 1) == server::Role::kActive);
    // gw3 unheard, still settling
    EXPECT_TRUE(fs_role(late, 2) == server::Role::kStandby);
}

// ---- plan 12 D1: planned migration ---------------------------------------------------

// gw1 hands F1 to gw2 through the store alone: owner record first, then the state
// drop and the fence release; gw1 refers clients to gw2 meanwhile; gw2's next tick
// takes it ahead of the node order (gw1 is first and alive), with gw1 as the node
// whose residue to clear and "migrate" as the reason.  A bystander gw3 leaves the
// export alone while the handover is pending.
TEST(FsClusterController, MigrateHandsOverViaOwnerRecord) {
    MemStore store;
    Exports exports({{"gw1", "gw3", "gw2"}, {"gw1"}, {"gw1"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    (void)store.put_node_address("gw3", "10.0.0.3:2049");
    core::FsOwnerView view1, view2, view3;
    FsRecorder rec1, rec2, rec3;
    server::FsClusterController gw1(aa_config("gw1"), exports.table, store, view1, rec1.hooks());
    server::FsClusterController gw2(aa_config("gw2"), exports.table, store, view2, rec2.hooks());
    server::FsClusterController gw3(aa_config("gw3"), exports.table, store, view3, rec3.hooks());
    gw1.tick();
    gw2.tick();
    gw3.tick();
    ASSERT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    EXPECT_TRUE(view_of(view2, 1).role == core::FsRole::kRemote);
    // Not the owner / unknown fsid / ourselves / a stranger as the target.
    auto not_owner = gw2.request_migrate(1, "gw3");
    ASSERT_TRUE(!not_owner.has_value());
    EXPECT_EQ(static_cast<int>(not_owner.error()), EPERM);
    auto unknown = gw1.request_migrate(9, "gw2");
    ASSERT_TRUE(!unknown.has_value());
    EXPECT_EQ(static_cast<int>(unknown.error()), EINVAL);
    auto self = gw1.request_migrate(1, "gw1");
    ASSERT_TRUE(!self.has_value());
    EXPECT_EQ(static_cast<int>(self.error()), EINVAL);
    auto stranger = gw1.request_migrate(1, "gw9");
    ASSERT_TRUE(!stranger.has_value());
    EXPECT_EQ(static_cast<int>(stranger.error()), EHOSTDOWN);
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    EXPECT_EQ(gw1.migrations(), 0u);

    store.log.clear();
    rec1.calls.clear();
    ASSERT_TRUE(gw1.request_migrate(1, "gw2").has_value());
    // Inline hooks: the whole source side ran.  Owner record before the fence release.
    EXPECT_STREQ(joined(store.log), "put_owner:1=gw2 release_fs:1");
    EXPECT_STREQ(joined(rec1.calls), "deactivate:1");
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    EXPECT_EQ(gw1.migrations(), 1u);
    EXPECT_STREQ(store.owners[1].node, "gw2");
    EXPECT_STREQ(store.owners[1].address, "10.0.0.2:2049");
    EXPECT_EQ(store.owners[1].fs_epoch, 1u);
    // F2/F3 only
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 2u);
    // gw1 refers to gw2 already (owner record, gw2's heartbeat live).
    const auto& one = view_of(view1, 1);
    EXPECT_TRUE(one.role == core::FsRole::kRemote);
    EXPECT_STREQ(one.node, "gw2");
    EXPECT_STREQ(one.address, "10.0.0.2:2049");
    EXPECT_EQ(one.fs_epoch, 1u);
    EXPECT_TRUE(fs_role(gw1, 2) == server::Role::kActive);
    // gw3 (ahead of gw2 in the list) sees the pending handover and yields.
    gw3.tick();
    EXPECT_TRUE(fs_role(gw3, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view3, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view3, 1).node, "gw2");
    EXPECT_TRUE(rec3.calls.empty());
    // gw1's own next tick does not take it back either (it is first in line).
    gw1.tick();
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    // gw2 takes it: storage eviction naming gw1, reason migrate, grace, owner record.
    store.log.clear();
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kActive);
    EXPECT_STREQ(joined(rec2.calls), "takeover:1 activate:1");
    ASSERT_TRUE(rec2.takeovers.size() == 1u);
    EXPECT_STREQ(rec2.takeovers[0].prev_node, "gw1");
    EXPECT_STREQ(rec2.takeovers[0].reason, "migrate");
    EXPECT_EQ(rec2.takeovers[0].identity.epoch, 2u);
    EXPECT_STREQ(joined(store.log), "renew_fences acquire_fs:1 fs_epoch:1 put_owner:1=gw2");
    EXPECT_EQ(store.owners[1].fs_epoch, 2u);
    EXPECT_TRUE(view_of(view2, 1).role == core::FsRole::kActive);
    // Everyone else now refers to gw2 by its live fence, with the new fs epoch.
    gw1.tick();
    gw3.tick();
    EXPECT_TRUE(view_of(view1, 1).role == core::FsRole::kRemote);
    EXPECT_EQ(view_of(view1, 1).fs_epoch, 2u);
    EXPECT_TRUE(view_of(view3, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view3, 1).node, "gw2");
    // An ordinary takeover still says so.
    ASSERT_TRUE(rec1.takeovers.size() >= 1u);
    EXPECT_STREQ(rec1.takeovers[0].reason, "takeover");
}

// A dead target is refused before anything moves; a target that dies right after the
// handover leaves the export unowned, and after 2 × ttl the ordinary rule (node order,
// gw1 first) takes it back.
TEST(FsClusterController, MigrateToDeadTargetFallsBack) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw1"}, {"gw1"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    core::FsOwnerView view;
    FsRecorder rec;
    core::ClusterConfig cfg = aa_config("gw1");
    // ttl 300 ms
    cfg.fence_lease_ms = 100;
    server::FsClusterController gw1(cfg, exports.table, store, view, rec.hooks());
    gw1.tick();
    ASSERT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    // gw2 registered but its heartbeat lapsed: EHOSTDOWN, nothing written or dropped.
    (void)store.renew_fences("gw2", 60s);
    store.age_out_node("gw2", 1000);
    store.log.clear();
    rec.calls.clear();
    auto dead = gw1.request_migrate(1, "gw2");
    ASSERT_TRUE(!dead.has_value());
    EXPECT_EQ(static_cast<int>(dead.error()), EHOSTDOWN);
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    EXPECT_TRUE(store.log.empty());
    EXPECT_TRUE(rec.calls.empty());
    // still ours
    EXPECT_STREQ(store.owners[1].node, "gw1");
    EXPECT_EQ(gw1.migrations(), 0u);

    // gw2 alive: the handover goes through; gw2 then dies without taking the export.
    (void)store.renew_fences("gw2", 60s);
    ASSERT_TRUE(gw1.request_migrate(1, "gw2").has_value());
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kRemote);
    store.age_out_node("gw2", 1000);
    gw1.tick();
    // No live target: Unowned, and gw1 (first in line, gw2 dead) takes it straight back —
    // an ordinary takeover, no residue to name (the record was released).
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    EXPECT_STREQ(rec.takeovers.back().reason, "takeover");
    EXPECT_STREQ(rec.takeovers.back().prev_node, "");
    EXPECT_EQ(view_of(view, 1).fs_epoch, 2u);

    // Alive but idle target (its process is stuck, heartbeat still written by another
    // instance, say): the source yields for 2 × ttl, then takes it back.
    (void)store.renew_fences("gw2", 60s);
    ASSERT_TRUE(gw1.request_migrate(1, "gw2").has_value());
    gw1.tick();
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kRemote);
    std::this_thread::sleep_for(700ms);
    gw1.tick();
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    EXPECT_EQ(gw1.migrations(), 2u);
}

// ---- plan 12 B3: the controller follows the export set ------------------------------

namespace {

// Publishes `plan` on `table` (no backends are started: MemoryBackend needs none) and
// returns the new set.
std::shared_ptr<const core::ExportSet> apply_plan(core::ExportTable& table, core::ExportSetPlan plan,
                                                  std::vector<uint32_t> add_fsids = {}) {
    std::vector<std::unique_ptr<backend::Backend>> started;
    for (uint32_t fsid : add_fsids) started.push_back(std::make_unique<backend::MemoryBackend>(fsid));
    auto applied = table.apply(std::move(plan), started, 1);
    if (!applied) return nullptr;
    return *applied;
}

core::ExportConfig export_cfg(uint32_t fsid, std::vector<std::string> nodes) {
    core::ExportConfig cfg;
    cfg.path = "/export/" + std::to_string(fsid);
    cfg.fsid = fsid;
    cfg.clients = {"127.0.0.0/8"};
    cfg.nodes = std::move(nodes);
    return cfg;
}

}  // namespace

// A new fsid enters Standby / Unowned and is taken on the next tick by the node order.
TEST(FsClusterController, SyncExportsAddsAndTakes) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw1"}, {"gw1"}});
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    ctl.tick();
    for (uint32_t f = 1; f <= 3; ++f) ASSERT_TRUE(fs_role(ctl, f) == server::Role::kActive);
    rec.calls.clear();

    core::ExportSetPlan plan;
    plan.add.push_back(export_cfg(4, {"gw1"}));
    auto set = apply_plan(exports.table, std::move(plan), {4});
    ASSERT_TRUE(set != nullptr);
    ctl.sync_exports(set);
    EXPECT_TRUE(fs_role(ctl, 4) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 4).role == core::FsRole::kUnowned);
    // nothing is taken at sync time
    EXPECT_TRUE(rec.calls.empty());
    auto snap = ctl.snapshot();
    ASSERT_TRUE(snap.size() == 4u);
    EXPECT_EQ(snap[3].fsid, 4u);
    EXPECT_STREQ(snap[3].path, "/export/4");
    EXPECT_STREQ(joined(snap[3].nodes), "gw1");

    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 4) == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "takeover:4 activate:4");
    EXPECT_TRUE(view_of(view, 4).role == core::FsRole::kActive);
    EXPECT_EQ(view_of(view, 4).fs_epoch, 1u);
    ASSERT_TRUE(store.fences["gw1"].holds.size() == 4u);
    EXPECT_STREQ(store.owners[4].node, "gw1");
    // The others were not touched.
    for (uint32_t f = 1; f <= 3; ++f) EXPECT_TRUE(fs_role(ctl, f) == server::Role::kActive);
    EXPECT_EQ(ctl.snapshot()[0].takeovers, 1u);

    // Syncing the same set again changes nothing.
    ctl.sync_exports(exports.table.snapshot());
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 4) == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "takeover:4 activate:4");
}

// The owner drops out of a changed `nodes`: the export is handed (owner record →
// first live listed node, local drain) and the target takes it as a migration.
TEST(FsClusterController, SyncExportsNodesChangeMigratesOwner) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw1"}, {"gw1"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    (void)store.put_node_address("gw3", "10.0.0.3:2049");
    core::FsOwnerView view1, view2;
    FsRecorder rec1, rec2;
    server::FsClusterController gw1(aa_config("gw1"), exports.table, store, view1, rec1.hooks());
    server::FsClusterController gw2(aa_config("gw2"), exports.table, store, view2, rec2.hooks());
    gw1.tick();
    gw2.tick();
    ASSERT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    ASSERT_TRUE(fs_role(gw2, 1) == server::Role::kStandby);
    rec1.calls.clear();
    rec2.calls.clear();

    // gw3 (dead: registered, no heartbeat) first, then gw2: the first *live* one wins.
    core::ExportSetPlan plan;
    plan.update.push_back(export_cfg(1, {"gw3", "gw2"}));
    auto set = apply_plan(exports.table, std::move(plan));
    ASSERT_TRUE(set != nullptr);
    // updated in place
    EXPECT_TRUE(set->by_fsid(1) == exports.table.snapshot()->by_fsid(1));
    store.log.clear();
    gw1.sync_exports(set);
    EXPECT_STREQ(joined(store.log), "put_owner:1=gw2 release_fs:1");
    EXPECT_STREQ(joined(rec1.calls), "deactivate:1");
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    EXPECT_EQ(gw1.migrations(), 1u);
    EXPECT_STREQ(store.owners[1].node, "gw2");
    // F2/F3 only
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 2u);
    EXPECT_TRUE(view_of(view1, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view1, 1).node, "gw2");
    EXPECT_STREQ(joined(gw1.snapshot()[0].nodes), "gw3 gw2");
    // gw1 does not take it back (not listed); gw2 takes it as a migration.
    gw1.tick();
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kStandby);
    gw2.sync_exports(set);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kActive);
    EXPECT_STREQ(joined(rec2.calls), "takeover:1 activate:1");
    ASSERT_TRUE(rec2.takeovers.size() == 1u);
    EXPECT_STREQ(rec2.takeovers[0].prev_node, "gw1");
    EXPECT_STREQ(rec2.takeovers[0].reason, "migrate");
    gw1.tick();
    EXPECT_TRUE(view_of(view1, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view1, 1).node, "gw2");
    EXPECT_EQ(view_of(view1, 1).fs_epoch, 2u);

    // A nodes change that keeps the owner listed only swaps the list.
    core::ExportSetPlan reorder;
    reorder.update.push_back(export_cfg(1, {"gw2", "gw1"}));
    set = apply_plan(exports.table, std::move(reorder));
    ASSERT_TRUE(set != nullptr);
    rec2.calls.clear();
    gw2.sync_exports(set);
    gw2.tick();
    EXPECT_TRUE(fs_role(gw2, 1) == server::Role::kActive);
    EXPECT_TRUE(rec2.calls.empty());
    EXPECT_STREQ(joined(gw2.snapshot()[0].nodes), "gw2 gw1");
}

// An fsid gone from the set: the owner drains it (deactivate hook, fence released,
// erased from the controller); a non-owner just forgets it.  The controller's
// reference kept the entry out of the retirement queue until the drain was done.
TEST(FsClusterController, SyncExportsRemovalDrainsOwner) {
    MemStore store;
    Exports exports({{"gw1", "gw2"}, {"gw1", "gw2"}, {"gw2", "gw1"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw2", "10.0.0.2:2049");
    core::FsOwnerView view1, view2;
    FsRecorder rec1, rec2;
    server::FsClusterController gw1(aa_config("gw1"), exports.table, store, view1, rec1.hooks());
    server::FsClusterController gw2(aa_config("gw2"), exports.table, store, view2, rec2.hooks());
    gw1.tick();
    gw2.tick();
    gw1.tick();
    ASSERT_TRUE(fs_role(gw1, 2) == server::Role::kActive);
    ASSERT_TRUE(fs_role(gw2, 3) == server::Role::kActive);
    EXPECT_TRUE(view_of(view1, 3).role == core::FsRole::kRemote);
    rec1.calls.clear();
    rec2.calls.clear();

    core::ExportSetPlan plan;
    plan.remove.push_back(2);
    auto old_set = exports.table.snapshot();
    auto set = apply_plan(exports.table, std::move(plan));
    ASSERT_TRUE(set != nullptr);
    old_set.reset();
    // Still referenced by both controllers: not retired yet.
    EXPECT_TRUE(exports.table.take_retired().empty());
    EXPECT_EQ(exports.table.retired_pending(), 1u);

    // The non-owner: erased silently, no hooks, no store writes.
    store.log.clear();
    gw2.sync_exports(set);
    EXPECT_TRUE(rec2.calls.empty());
    // a no-op release of a hold it never had
    EXPECT_STREQ(joined(store.log), "release_fs:2");
    EXPECT_TRUE(gw2.snapshot().size() == 2u);
    EXPECT_TRUE(view2.snapshot()->find(2) == view2.snapshot()->end());
    // gw1 still holds it
    EXPECT_TRUE(exports.table.take_retired().empty());

    // The owner: drained inline (deactivate, fence released), then gone.
    store.log.clear();
    gw1.sync_exports(set);
    EXPECT_STREQ(joined(rec1.calls), "deactivate:2");
    EXPECT_STREQ(joined(store.log), "release_fs:2");
    // unknown fsid reads as Standby
    EXPECT_TRUE(fs_role(gw1, 2) == server::Role::kStandby);
    auto snap = gw1.snapshot();
    ASSERT_TRUE(snap.size() == 2u);
    EXPECT_EQ(snap[0].fsid, 1u);
    EXPECT_EQ(snap[1].fsid, 3u);
    EXPECT_TRUE(view1.snapshot()->find(2) == view1.snapshot()->end());
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 1u);
    EXPECT_EQ(store.fences["gw1"].holds[0].fsid, 1u);
    EXPECT_TRUE(fs_role(gw1, 1) == server::Role::kActive);
    // Now nothing references the entry: the table hands it out for stop().
    auto retired = exports.table.take_retired();
    ASSERT_TRUE(retired.size() == 1u);
    EXPECT_EQ(retired[0]->fsid, 2u);
    // Later ticks do not resurrect it.
    gw1.tick();
    gw2.tick();
    EXPECT_TRUE(gw1.snapshot().size() == 2u);
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 1u);
    EXPECT_TRUE(rec1.calls.size() == 1u);
    // Operator requests for the removed fsid: unknown.
    auto take = gw1.request_takeover(2, false);
    ASSERT_TRUE(!take.has_value());
    EXPECT_EQ(static_cast<int>(take.error()), EINVAL);

    // Removed while Activating: the activation is abandoned and the export drained.
    // (fsid 3 is gw2's; gw2 dies, gw1 starts taking it, the set drops it meanwhile.)
    store.age_out_node("gw2");
    std::vector<std::function<void()>> posted;
    FsRecorder rec1b;
    auto hooks = rec1b.hooks();
    hooks.post = [&](std::function<void()> fn) { posted.push_back(std::move(fn)); };
    core::FsOwnerView view1b;
    server::FsClusterController gw1b(aa_config("gw1"), exports.table, store, view1b, std::move(hooks));
    // F1 and F3 taken: activations posted, not yet run
    gw1b.tick();
    ASSERT_TRUE(fs_role(gw1b, 3) == server::Role::kActivating);
    core::ExportSetPlan drop3;
    drop3.remove.push_back(3);
    set = apply_plan(exports.table, std::move(drop3));
    ASSERT_TRUE(set != nullptr);
    gw1b.sync_exports(set);
    EXPECT_TRUE(fs_role(gw1b, 3) == server::Role::kDraining);
    // the activation finds it Draining and steps aside
    for (auto& fn : posted) fn();
    posted.clear();
    EXPECT_TRUE(gw1b.snapshot().size() == 1u);
    EXPECT_EQ(gw1b.snapshot()[0].fsid, 1u);
    EXPECT_EQ(count(rec1b.calls, "deactivate:3"), 1u);
    EXPECT_EQ(count(rec1b.calls, "activate:3"), 0u);
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 1u);
}

// The owner is dropped from `nodes` but no listed node is alive: it keeps serving
// (warning each tick) and hands over as soon as one appears.
TEST(FsClusterController, SyncExportsNodesChangeNoCandidateKeeps) {
    MemStore store;
    Exports exports({{"gw1"}, {"gw1"}, {"gw1"}});
    (void)store.put_node_address("gw1", "10.0.0.1:2049");
    (void)store.put_node_address("gw3", "10.0.0.3:2049");
    core::FsOwnerView view;
    FsRecorder rec;
    server::FsClusterController ctl(aa_config("gw1"), exports.table, store, view, rec.hooks());
    ctl.tick();
    ASSERT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    rec.calls.clear();

    // gw3 is registered but has no heartbeat; gw9 is unknown altogether.
    core::ExportSetPlan plan;
    plan.update.push_back(export_cfg(1, {"gw9", "gw3"}));
    auto set = apply_plan(exports.table, std::move(plan));
    ASSERT_TRUE(set != nullptr);
    store.log.clear();
    ctl.sync_exports(set);
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_EQ(ctl.migrations(), 0u);
    EXPECT_TRUE(store.owners[1].node == "gw1");
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kActive);
    EXPECT_STREQ(joined(ctl.snapshot()[0].nodes), "gw9 gw3");
    // Ticks keep it (and keep looking): no drain, no owner change.
    ctl.tick();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_TRUE(store.owners[1].node == "gw1");
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 3u);

    // gw3 comes up (heartbeat live): the next tick hands the export over.
    (void)store.renew_fences("gw3", 60s);
    store.log.clear();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_STREQ(joined(rec.calls), "deactivate:1");
    EXPECT_EQ(ctl.migrations(), 1u);
    EXPECT_STREQ(store.owners[1].node, "gw3");
    EXPECT_STREQ(store.owners[1].address, "10.0.0.3:2049");
    EXPECT_TRUE(store.fences["gw1"].holds.size() == 2u);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kRemote);
    EXPECT_STREQ(view_of(view, 1).node, "gw3");
    // Not listed: never taken back, even once gw3 dies and the export sits unowned.
    store.age_out_node("gw3");
    for (int i = 0; i < 3; ++i) ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kStandby);
    EXPECT_TRUE(view_of(view, 1).role == core::FsRole::kUnowned);

    // The operator's way out (ctl takeover from outside the list) stays put: taken by
    // request, not by a nodes change, the export is not handed over on later ticks.
    rec.calls.clear();
    ASSERT_TRUE(ctl.request_takeover(1, false).has_value());
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    // a listed node alive again changes nothing
    (void)store.renew_fences("gw3", 60s);
    ctl.tick();
    ctl.tick();
    EXPECT_TRUE(fs_role(ctl, 1) == server::Role::kActive);
    EXPECT_STREQ(joined(rec.calls), "takeover:1 activate:1");
    EXPECT_EQ(ctl.migrations(), 1u);
    EXPECT_STREQ(store.owners[1].node, "gw1");
}

// ---- plan 12 C2: polling the catalog and applying it ----------------------------

namespace {

std::string catalog_doc(uint64_t version, const std::string& exports) {
    return "[catalog]\nversion = " + std::to_string(version) + "\n" + exports;
}

std::string export_block(uint32_t fsid, const std::string& path, const std::string& nodes = "[\"gw1\"]",
                         const std::string& extra = "") {
    return "[[export]]\npath = \"" + path + "\"\nfsid = " + std::to_string(fsid) +
           "\nbackend = \"local\"\nclients = [\"127.0.0.0/8\"]\nnodes = " + nodes + "\n" + extra;
}

// A catalog-mode gateway in a box: the memory store, this host's config, the table
// booted from the store, an active-active controller polling the catalog after every
// tick, and the applier with inline posts and counted backend starts / stops.
struct CatalogGateway {
    MemStore store;
    std::string dir;
    core::Config local;
    std::unique_ptr<core::ExportTable> table;
    core::FsOwnerView view;
    FsRecorder rec;
    std::optional<server::FsClusterController> ctl;
    std::optional<server::CatalogApplier> applier;
    size_t posts = 0, starts = 0, stops = 0;
    std::vector<uint32_t> started, stopped;
    // start_backend fails for this fsid
    uint32_t fail_start_fsid = 0;

    // `first` builds v1's [[export]] blocks from the box's directory.
    CatalogGateway(const std::string& refresh, const std::function<std::string(const std::string&)>& first) {
        char tmpl[] = "/tmp/lnfs-cat-XXXXXX";
        dir = mkdtemp(tmpl);
        for (const char* sub : {"a", "b", "c"}) std::filesystem::create_directories(dir + "/" + sub);
        store.catalog_docs[1] = catalog_doc(1, first(dir));
        (void)store.put_node_address("gw1", "10.0.0.1:2049");
        auto parsed = core::parse_config(
            "[cluster]\nenabled = true\nid = \"cluster-ctrl-test\"\nshared_dir = \"/srv/shared\"\n"
            "mode = \"active-active\"\nnode = \"gw1\"\nnode_address = \"10.0.0.1:2049\"\n"
            "exports_source = \"catalog\"\ncatalog_refresh = \"" +
            refresh + "\"\n");
        ASSERT_TRUE(parsed.has_value());
        local = *parsed;
        auto boot = server::load_catalog_exports(store, local);
        ASSERT_TRUE(boot.has_value());
        auto built = core::ExportTable::build(local);
        ASSERT_TRUE(built.has_value());
        table = std::move(*built);
        // CoreState::local_config: the host's side only
        local.exports.clear();
        local.exports_from_catalog = false;
        auto hooks = rec.hooks();
        hooks.after_tick = [this] {
            if (applier) applier->poll();
        };
        core::ClusterConfig cfg = local.cluster;
        cfg.fence_lease_ms = 1000;
        ctl.emplace(cfg, *table, store, view, std::move(hooks));
        applier.emplace(server::CatalogApplier::Deps{.store = store,
                                                     .exports = *table,
                                                     .local = local,
                                                     .node = "gw1",
                                                     .fs_cluster = &*ctl,
                                                     .post =
                                                         [this](std::function<void()> fn) {
                                                             ++posts;
                                                             fn();
                                                         },
                                                     .start_backend = [this](backend::Backend& b) -> Result<void> {
                                                         ++starts;
                                                         if (b.fsid() == fail_start_fsid) return Err(errno_from(EIO));
                                                         started.push_back(static_cast<uint32_t>(b.fsid()));
                                                         return {};
                                                     },
                                                     .stop_backend =
                                                         [this](backend::Backend& b) {
                                                             ++stops;
                                                             stopped.push_back(static_cast<uint32_t>(b.fsid()));
                                                         },
                                                     .retire_overdue = std::chrono::milliseconds(1)},
                        boot->version, boot->catalog, boot->digest);
    }
    ~CatalogGateway() {
        applier.reset();
        ctl.reset();
        std::filesystem::remove_all(dir);
    }
    void publish(uint64_t expected, const std::string& exports) {
        auto wrote = store.write_catalog(expected, catalog_doc(expected + 1, exports));
        ASSERT_TRUE(wrote.has_value());
    }
};

}  // namespace

// A newer catalog on a tick: one apply is posted (never two), the new export joins
// the table and the controller and is taken on the next tick; a removal drains the
// export and the retirement sweep, posted by a later tick, stops its backend.
TEST(FsClusterController, CatalogAutoApplyOnTick) {
    CatalogGateway gw("auto", [](const std::string& d) { return export_block(1, d + "/a"); });
    EXPECT_EQ(gw.applier->applied(), 1u);
    EXPECT_EQ(gw.table->size(), 1u);
    gw.ctl->tick();
    ASSERT_TRUE(fs_role(*gw.ctl, 1) == server::Role::kActive);
    // v1 is current: nothing to post
    EXPECT_EQ(gw.posts, 0u);
    EXPECT_EQ(gw.applier->pending(), 0u);

    // v2 adds fsid 2.
    gw.publish(1, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b"));
    gw.ctl->tick();
    EXPECT_EQ(gw.posts, 1u);
    EXPECT_EQ(gw.applier->applied(), 2u);
    EXPECT_EQ(gw.applier->latest(), 2u);
    EXPECT_EQ(gw.applier->pending(), 0u);
    EXPECT_FALSE(gw.applier->applying());
    EXPECT_EQ(gw.applier->applies(), 1u);
    EXPECT_EQ(gw.applier->failures(), 0u);
    EXPECT_STREQ(gw.applier->last_error(), "");
    // one consistent reading for status / metrics (plan 12 C3)
    const auto status = gw.applier->status();
    EXPECT_EQ(status.applied, 2u);
    EXPECT_EQ(status.latest, 2u);
    EXPECT_EQ(status.pending, 0u);
    EXPECT_EQ(status.applies, 1u);
    EXPECT_STREQ(status.refresh, "auto");
    EXPECT_EQ(gw.table->size(), 2u);
    ASSERT_TRUE(gw.table->by_fsid(2) != nullptr);
    EXPECT_STREQ(gw.table->by_fsid(2)->path, gw.dir + "/b");
    EXPECT_EQ(gw.starts, 1u);
    EXPECT_TRUE(gw.started == std::vector<uint32_t>{2});
    // Recorded, and the controller knows the export (Standby until the next tick).
    ASSERT_TRUE(gw.store.catalog_applied.contains("gw1"));
    EXPECT_EQ(gw.store.catalog_applied["gw1"].version, 2u);
    EXPECT_STREQ(gw.store.catalog_applied["gw1"].status, "ok");
    EXPECT_STREQ(gw.store.catalog_applied["gw1"].digest, gw.applier->digest());
    EXPECT_TRUE(gw.ctl->snapshot().size() == 2u);
    EXPECT_TRUE(fs_role(*gw.ctl, 2) == server::Role::kStandby);
    gw.ctl->tick();
    EXPECT_TRUE(fs_role(*gw.ctl, 2) == server::Role::kActive);
    // nothing new: no second post
    EXPECT_EQ(gw.posts, 1u);
    EXPECT_EQ(count(gw.rec.calls, "activate:2"), 1u);

    // v3 drops fsid 1: drained on the apply, its backend stopped by the sweep the next
    // tick posts (the controller's drain released the last reference).
    gw.publish(2, export_block(2, gw.dir + "/b"));
    gw.ctl->tick();
    EXPECT_EQ(gw.applier->applied(), 3u);
    EXPECT_EQ(gw.posts, 2u);
    EXPECT_EQ(gw.table->size(), 1u);
    EXPECT_TRUE(gw.table->by_fsid(1) == nullptr);
    EXPECT_EQ(count(gw.rec.calls, "deactivate:1"), 1u);
    EXPECT_TRUE(gw.ctl->snapshot().size() == 1u);
    EXPECT_TRUE(gw.store.fences["gw1"].holds.size() == 1u);
    EXPECT_EQ(gw.table->retired_pending(), 1u);
    EXPECT_EQ(gw.stops, 0u);
    gw.ctl->tick();
    // the retirement sweep
    EXPECT_EQ(gw.posts, 3u);
    EXPECT_EQ(gw.stops, 1u);
    EXPECT_TRUE(gw.stopped == std::vector<uint32_t>{1});
    EXPECT_EQ(gw.table->retired_pending(), 0u);
    gw.ctl->tick();
    EXPECT_EQ(gw.posts, 3u);
    EXPECT_TRUE(fs_role(*gw.ctl, 2) == server::Role::kActive);

    // v4 changes fsid 2 in place (readonly + nodes): no backend churn, the same entry.
    const auto* two = gw.table->by_fsid(2);
    gw.publish(3, export_block(2, gw.dir + "/b", "[\"gw1\", \"gw2\"]", "readonly = true\n"));
    gw.ctl->tick();
    EXPECT_EQ(gw.applier->applied(), 4u);
    EXPECT_TRUE(gw.table->by_fsid(2) == two);
    EXPECT_TRUE(two->readonly.load());
    EXPECT_STREQ(joined(gw.ctl->snapshot()[0].nodes), "gw1 gw2");
    EXPECT_EQ(gw.starts, 1u);
    EXPECT_EQ(gw.stops, 1u);
    EXPECT_TRUE(fs_role(*gw.ctl, 2) == server::Role::kActive);
}

// catalog_refresh = "manual": the tick only records the newer version; apply_latest
// (ctl / reload / SIGHUP) applies it and clears the pending mark.
TEST(FsClusterController, CatalogManualLeavesPending) {
    CatalogGateway gw("manual", [](const std::string& d) { return export_block(1, d + "/a"); });
    gw.ctl->tick();
    gw.publish(1, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b"));
    gw.ctl->tick();
    gw.ctl->tick();
    EXPECT_EQ(gw.posts, 0u);
    EXPECT_EQ(gw.applier->applied(), 1u);
    EXPECT_EQ(gw.applier->latest(), 2u);
    EXPECT_EQ(gw.applier->pending(), 2u);
    EXPECT_STREQ(gw.applier->status().refresh, "manual");
    EXPECT_EQ(gw.table->size(), 1u);
    EXPECT_TRUE(gw.store.catalog_applied.empty());

    auto applied = gw.applier->apply_latest();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 2u);
    EXPECT_EQ(gw.applier->applied(), 2u);
    EXPECT_EQ(gw.applier->pending(), 0u);
    EXPECT_EQ(gw.table->size(), 2u);
    EXPECT_EQ(gw.store.catalog_applied["gw1"].version, 2u);
    // Nothing newer: apply_latest is a no-op that reports the current version.
    applied = gw.applier->apply_latest();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 2u);
    EXPECT_EQ(gw.starts, 1u);
    // apply_now goes through the post hook (the main loop in the daemon).
    gw.publish(2, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b") + export_block(3, gw.dir + "/c"));
    auto now = gw.applier->apply_now();
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(*now, 3u);
    EXPECT_EQ(gw.posts, 1u);
    EXPECT_EQ(gw.table->size(), 3u);
    // Flipping the key to auto (a reload) makes the next tick apply by itself.
    gw.local.cluster.catalog_refresh = "auto";
    gw.publish(3, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b"));
    gw.ctl->tick();
    EXPECT_EQ(gw.applier->applied(), 4u);
    EXPECT_EQ(gw.table->size(), 2u);
}

// A version this host cannot apply leaves the running set untouched, records
// catalog.<node> = <old> error:<why>, and is retried once fixed.
TEST(FsClusterController, CatalogApplyFailureKeepsOld) {
    CatalogGateway gw("auto", [](const std::string& d) { return export_block(1, d + "/a"); });
    gw.ctl->tick();
    auto before = gw.table->snapshot();

    // fsid 2 at a path this host does not have.
    gw.publish(1, export_block(1, gw.dir + "/a") + export_block(2, "/nonexistent/lnfs-c2"));
    gw.ctl->tick();
    EXPECT_EQ(gw.posts, 1u);
    EXPECT_EQ(gw.applier->applied(), 1u);
    EXPECT_EQ(gw.applier->latest(), 2u);
    EXPECT_EQ(gw.applier->pending(), 2u);
    EXPECT_EQ(gw.applier->applies(), 0u);
    EXPECT_EQ(gw.applier->failures(), 1u);
    EXPECT_TRUE(gw.applier->last_error().find("catalog v2") != std::string::npos);
    EXPECT_TRUE(gw.applier->last_error().find("fsid=2") != std::string::npos);
    // nothing published
    EXPECT_TRUE(gw.table->snapshot() == before);
    EXPECT_EQ(gw.starts, 0u);
    EXPECT_EQ(gw.store.catalog_applied["gw1"].version, 1u);
    EXPECT_TRUE(gw.store.catalog_applied["gw1"].status.starts_with("error:catalog v2"));
    EXPECT_STREQ(gw.store.catalog_applied["gw1"].digest, gw.applier->digest());
    // The failed version is not retried by the next ticks (every kRetryEveryPolls polls
    // only): no post, the pending mark stays; a manual apply retries at once and fails
    // the same way.
    gw.ctl->tick();
    gw.ctl->tick();
    EXPECT_EQ(gw.posts, 1u);
    EXPECT_EQ(gw.applier->failures(), 1u);
    EXPECT_EQ(gw.applier->pending(), 2u);
    EXPECT_TRUE(fs_role(*gw.ctl, 1) == server::Role::kActive);
    auto again = gw.applier->apply_latest();
    ASSERT_TRUE(!again.has_value());
    EXPECT_EQ(gw.applier->failures(), 2u);
    for (int i = 0; i < server::CatalogApplier::kRetryEveryPolls; ++i) gw.ctl->tick();
    // the automatic retry came around once
    EXPECT_EQ(gw.posts, 2u);
    EXPECT_EQ(gw.applier->failures(), 3u);

    // A rejected change: fsid 1's path moved.
    gw.publish(2, export_block(1, gw.dir + "/c"));
    auto rejected = gw.applier->apply_latest();
    ASSERT_TRUE(!rejected.has_value());
    EXPECT_EQ(static_cast<int>(rejected.error()), EINVAL);
    EXPECT_TRUE(gw.applier->last_error().find("remove and re-add") != std::string::npos);
    EXPECT_TRUE(gw.applier->last_error().find("fsid 1") != std::string::npos);
    EXPECT_TRUE(gw.table->snapshot() == before);
    EXPECT_EQ(gw.applier->applied(), 1u);

    // Two additions, the second fails to start: the first, already started, is stopped
    // again and nothing is published.
    gw.publish(3, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b") + export_block(3, gw.dir + "/c"));
    gw.fail_start_fsid = 3;
    auto failed = gw.applier->apply_latest();
    ASSERT_TRUE(!failed.has_value());
    EXPECT_EQ(static_cast<int>(failed.error()), EIO);
    EXPECT_TRUE(gw.applier->last_error().find("fsid=3") != std::string::npos);
    EXPECT_TRUE(gw.applier->last_error().find("failed to start") != std::string::npos);
    EXPECT_EQ(gw.starts, 2u);
    EXPECT_TRUE(gw.started == std::vector<uint32_t>{2});
    EXPECT_EQ(gw.stops, 1u);
    EXPECT_TRUE(gw.stopped == std::vector<uint32_t>{2});
    EXPECT_TRUE(gw.table->snapshot() == before);
    EXPECT_EQ(gw.table->size(), 1u);
    gw.fail_start_fsid = 0;

    // Fixed (v5): applied, the failure record replaced.
    gw.publish(4, export_block(1, gw.dir + "/a") + export_block(2, gw.dir + "/b"));
    before.reset();
    gw.ctl->tick();
    EXPECT_EQ(gw.applier->applied(), 5u);
    EXPECT_EQ(gw.applier->pending(), 0u);
    EXPECT_STREQ(gw.applier->last_error(), "");
    EXPECT_EQ(gw.table->size(), 2u);
    EXPECT_EQ(gw.store.catalog_applied["gw1"].version, 5u);
    EXPECT_STREQ(gw.store.catalog_applied["gw1"].status, "ok");
    EXPECT_EQ(gw.starts, 3u);
    gw.ctl->tick();
    EXPECT_TRUE(fs_role(*gw.ctl, 2) == server::Role::kActive);

    // The catalog disappearing from the store is not an error: nothing to apply.
    gw.store.catalog_docs.clear();
    gw.ctl->tick();
    EXPECT_EQ(gw.applier->applied(), 5u);
    EXPECT_EQ(gw.applier->pending(), 0u);
    // v2 three times, the rejected v3, the failed start
    EXPECT_EQ(gw.applier->failures(), 5u);
    // only v5 went through; nothing in the store means nothing seen
    EXPECT_EQ(gw.applier->applies(), 1u);
    EXPECT_EQ(gw.applier->latest(), 0u);
}

// The failover controller polls the same way: a standby applies too, so the table
// its next activation rebuilds the stack from is current.
TEST(ClusterController, CatalogPollAfterTick) {
    MemStore store;
    size_t polls = 0;
    Recorder rec;
    auto hooks = rec.hooks();
    hooks.after_tick = [&] { ++polls; };
    server::ClusterController ctl(config("gw1", "standby"), store, std::move(hooks));
    ctl.tick();
    ctl.tick();
    EXPECT_EQ(polls, 2u);
    EXPECT_TRUE(ctl.role() == server::Role::kStandby);
    // an unreadable store still polls
    store.fail_read = errno_from(EIO);
    ctl.tick();
    EXPECT_EQ(polls, 3u);
}
