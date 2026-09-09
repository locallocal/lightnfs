#include "server/cluster_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

#include "core/atomic_file.hpp"
#include "core/catalog.hpp"
#include "core/file_handle.hpp"
#include "util/log.hpp"

namespace lnfs::server {
namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Same hash and file naming as StateMgr::persist_client (state_mgr.cpp) so a reclaim
// list can move between state_dir/clients/ and the shared directory unchanged.
uint64_t fnv64(std::string_view bytes) {
    uint64_t h = 1469598103934665603ull;
    for (char c : bytes) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string client_file_name(std::string_view owner_id) {
    char name[24];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(fnv64(owner_id)));
    return name;
}

Result<uint64_t> parse_u64(std::string_view text) {
    uint64_t out = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc{} || end == text.data()) return Err(errno_from(EINVAL));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

// How long a writer waits for a live lock held by another gateway before EBUSY.
constexpr std::chrono::milliseconds kLockWait{100};

class PosixClusterStore final : public ClusterStore {
 public:
    PosixClusterStore(std::string dir, std::chrono::milliseconds stale_lock_after)
        : dir_(std::move(dir)), stale_lock_after_(stale_lock_after) {}

    Result<std::array<std::byte, 16>> load_or_create_key() override {
        LNFS_TRY(ensure_layout());
        return core::load_or_create_hmac_key(dir_ + "/hmac.key");
    }

    Result<uint64_t> read_epoch() override {
        auto text = LNFS_TRY(core::read_file_if_exists(dir_ + "/epoch"));
        if (!text) return uint64_t{0};
        return parse_u64(trim(*text));
    }

    Result<uint64_t> bump_epoch() override {
        LNFS_TRY(ensure_layout());
        auto guard = LNFS_TRY(lock("epoch"));
        uint64_t epoch = LNFS_TRY(read_epoch()) + 1;
        LNFS_TRY(core::atomic_write_file(dir_ + "/epoch", std::to_string(epoch) + "\n"));
        return epoch;
    }

    Result<std::vector<std::string>> list_clients() override { return list_clients_in(dir_ + "/clients"); }

    Result<void> put_client(std::string_view owner_id) override {
        LNFS_TRY(ensure_layout());
        return put_client_in(dir_ + "/clients", owner_id);
    }

    Result<void> erase_client(std::string_view owner_id) override {
        return erase_client_in(dir_ + "/clients", owner_id);
    }

    Result<std::optional<FenceRecord>> read_fence() override {
        auto text = LNFS_TRY(core::read_file_if_exists(dir_ + "/fence"));
        if (!text) return std::optional<FenceRecord>{};
        return std::optional<FenceRecord>{LNFS_TRY(parse_fence(*text))};
    }

    Result<FenceRecord> acquire_fence(std::string_view node, uint64_t epoch, std::chrono::milliseconds ttl,
                                      bool force) override {
        LNFS_TRY(ensure_layout());
        auto guard = LNFS_TRY(lock("fence"));
        auto current = LNFS_TRY(read_fence());
        if (current && !force && current->node != node && !expired(*current)) return Err(errno_from(EBUSY));
        FenceRecord rec{std::string(node), epoch, now_ms() + ttl.count()};
        LNFS_TRY(write_fence(rec));
        return rec;
    }

    Result<void> renew_fence(std::string_view node, std::chrono::milliseconds ttl) override {
        auto guard = LNFS_TRY(lock("fence"));
        auto current = LNFS_TRY(read_fence());
        if (!current || current->node != node) return Err(errno_from(EPERM));
        current->expires_at_ms = now_ms() + ttl.count();
        return write_fence(*current);
    }

    Result<void> release_fence(std::string_view node) override {
        auto guard = LNFS_TRY(lock("fence"));
        auto current = LNFS_TRY(read_fence());
        if (!current) return {};
        if (current->node != node) return Err(errno_from(EPERM));
        if (::unlink((dir_ + "/fence").c_str()) < 0 && errno != ENOENT) return Err(errno_from(errno));
        return {};
    }

    Result<void> put_exports_digest(std::string_view node, std::string_view digest) override {
        LNFS_TRY(ensure_layout());
        return core::atomic_write_file(dir_ + "/exports." + std::string(node), std::string(digest) + "\n");
    }

    Result<std::vector<std::pair<std::string, std::string>>> list_exports_digests() override {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir_, ec);
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            auto name = entry.path().filename().string();
            if (!name.starts_with("exports.") || name.find(".tmp.") != std::string::npos) continue;
            if (!entry.is_regular_file(ec)) continue;
            auto text = core::read_file_if_exists(entry.path().string());
            if (!text || !*text) continue;
            out.emplace_back(name.substr(sizeof("exports.") - 1), std::string(trim(**text)));
        }
        if (ec) return Err(errno_from(ec.value()));
        return out;
    }

    // ---- active-active (plan 12 A2) ----------------------------------------------------

    Result<uint64_t> read_node_epoch(std::string_view node) override {
        return read_counter(dir_ + "/epoch." + std::string(node));
    }

    Result<uint64_t> bump_node_epoch(std::string_view node) override {
        LNFS_TRY(ensure_layout());
        std::string name = "epoch." + std::string(node);
        auto guard = LNFS_TRY(lock(name.c_str()));
        return bump_counter(dir_ + "/" + name);
    }

    Result<void> put_node_address(std::string_view node, std::string_view address) override {
        LNFS_TRY(ensure_dir(dir_ + "/nodes"));
        return core::atomic_write_file(dir_ + "/nodes/" + std::string(node), std::string(address) + "\n");
    }

    Result<std::vector<std::pair<std::string, std::string>>> list_nodes() override {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir_ + "/nodes", ec);
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            auto name = entry.path().filename().string();
            if (name.find(".tmp.") != std::string::npos || !entry.is_regular_file(ec)) continue;
            auto text = core::read_file_if_exists(entry.path().string());
            if (!text || !*text) continue;
            out.emplace_back(name, std::string(trim(**text)));
        }
        if (ec) return Err(errno_from(ec.value()));
        std::sort(out.begin(), out.end());
        return out;
    }

    Result<std::optional<FenceRecord>> read_fs_fence(uint32_t fsid) override {
        auto records = LNFS_TRY(list_fences());
        // a live record wins; else the latest expired one
        std::optional<FenceRecord> best;
        for (const auto& rec : records) {
            const NodeFences::Hold* hold = find_hold(rec, fsid);
            if (!hold) continue;
            FenceRecord candidate{rec.node, hold->epoch, rec.expires_at_ms};
            if (!best || (expired(*best) && (!expired(candidate) || candidate.expires_at_ms > best->expires_at_ms)))
                best = std::move(candidate);
        }
        return best;
    }

    Result<FenceRecord> acquire_fs_fence(uint32_t fsid, std::string_view node, uint64_t epoch,
                                         std::chrono::milliseconds ttl, bool force) override {
        LNFS_TRY(ensure_layout());
        auto guard = LNFS_TRY(lock("fence"));
        auto records = LNFS_TRY(list_fences());
        NodeFences mine{std::string(node), 0, {}};
        for (auto& rec : records) {
            if (rec.node == node) {
                mine = rec;
                continue;
            }
            const NodeFences::Hold* hold = find_hold(rec, fsid);
            if (!hold) continue;
            if (!force && !expired(FenceRecord{rec.node, hold->epoch, rec.expires_at_ms}))
                return Err(errno_from(EBUSY));
        }
        // One export is never named by two records: strip it from everyone else's.
        for (auto& rec : records) {
            if (rec.node == node || !find_hold(rec, fsid)) continue;
            std::erase_if(rec.holds, [&](const NodeFences::Hold& h) { return h.fsid == fsid; });
            LNFS_TRY(write_node_fences(rec));
        }
        std::erase_if(mine.holds, [&](const NodeFences::Hold& h) { return h.fsid == fsid; });
        mine.holds.push_back({fsid, epoch});
        std::sort(mine.holds.begin(), mine.holds.end(), [](const auto& a, const auto& b) { return a.fsid < b.fsid; });
        mine.expires_at_ms = now_ms() + ttl.count();
        LNFS_TRY(write_node_fences(mine));
        return FenceRecord{std::string(node), epoch, mine.expires_at_ms};
    }

    Result<void> renew_fences(std::string_view node, std::chrono::milliseconds ttl) override {
        LNFS_TRY(ensure_layout());
        auto guard = LNFS_TRY(lock("fence"));
        auto current = LNFS_TRY(read_node_fences(node));
        NodeFences mine = current ? std::move(*current) : NodeFences{std::string(node), 0, {}};
        mine.expires_at_ms = now_ms() + ttl.count();
        return write_node_fences(mine);
    }

    Result<void> release_fs_fence(uint32_t fsid, std::string_view node) override {
        auto guard = LNFS_TRY(lock("fence"));
        auto records = LNFS_TRY(list_fences());
        for (auto& rec : records) {
            if (!find_hold(rec, fsid)) continue;
            if (rec.node != node) return Err(errno_from(EPERM));
            std::erase_if(rec.holds, [&](const NodeFences::Hold& h) { return h.fsid == fsid; });
            return write_node_fences(rec);
        }
        // nobody holds it
        return {};
    }

    Result<std::vector<NodeFences>> list_fences() override {
        std::vector<NodeFences> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir_, ec);
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            auto name = entry.path().filename().string();
            if (!name.starts_with("fence.") || name.ends_with(".lock") || name.find(".tmp.") != std::string::npos ||
                !entry.is_regular_file(ec))
                continue;
            auto text = core::read_file_if_exists(entry.path().string());
            if (!text || !*text) continue;
            out.push_back(LNFS_TRY(parse_node_fences(name.substr(sizeof("fence.") - 1), **text)));
        }
        if (ec) return Err(errno_from(ec.value()));
        std::sort(out.begin(), out.end(), [](const NodeFences& a, const NodeFences& b) { return a.node < b.node; });
        return out;
    }

    Result<uint64_t> read_fs_epoch(uint32_t fsid) override { return read_counter(fs_dir(fsid) + "/epoch"); }

    Result<uint64_t> bump_fs_epoch(uint32_t fsid) override {
        LNFS_TRY(ensure_dir(fs_dir(fsid)));
        std::string name = "fs/" + std::to_string(fsid) + "/epoch";
        auto guard = LNFS_TRY(lock(name.c_str()));
        return bump_counter(fs_dir(fsid) + "/epoch");
    }

    Result<std::optional<OwnerRecord>> read_owner(uint32_t fsid) override {
        auto text = LNFS_TRY(core::read_file_if_exists(fs_dir(fsid) + "/owner"));
        if (!text) return std::optional<OwnerRecord>{};
        // "<fs_epoch> <address> <node>": the node is the rest of the line.
        std::string_view line = trim(*text);
        size_t a = line.find(' ');
        if (a == std::string_view::npos) return Err(errno_from(EINVAL));
        size_t b = line.find(' ', a + 1);
        if (b == std::string_view::npos) return Err(errno_from(EINVAL));
        OwnerRecord rec;
        rec.fs_epoch = LNFS_TRY(parse_u64(line.substr(0, a)));
        rec.address = std::string(line.substr(a + 1, b - a - 1));
        rec.node = std::string(line.substr(b + 1));
        if (rec.node.empty()) return Err(errno_from(EINVAL));
        return std::optional<OwnerRecord>{std::move(rec)};
    }

    Result<void> put_owner(uint32_t fsid, const OwnerRecord& owner) override {
        if (owner.node.empty() || owner.address.find(' ') != std::string::npos) return Err(errno_from(EINVAL));
        LNFS_TRY(ensure_dir(fs_dir(fsid)));
        return core::atomic_write_file(fs_dir(fsid) + "/owner",
                                       std::to_string(owner.fs_epoch) + " " + owner.address + " " + owner.node + "\n");
    }

    Result<std::vector<std::string>> list_clients(uint32_t fsid) override {
        return list_clients_in(fs_dir(fsid) + "/clients");
    }

    Result<void> put_client(uint32_t fsid, std::string_view owner_id) override {
        LNFS_TRY(ensure_dir(fs_dir(fsid) + "/clients"));
        return put_client_in(fs_dir(fsid) + "/clients", owner_id);
    }

    Result<void> erase_client(uint32_t fsid, std::string_view owner_id) override {
        return erase_client_in(fs_dir(fsid) + "/clients", owner_id);
    }

    // ---- shared export catalog (plan 12 A3) --------------------------------------------

    Result<std::optional<CatalogDoc>> read_catalog() override {
        auto text = LNFS_TRY(core::read_file_if_exists(dir_ + "/catalog.toml"));
        if (!text) return std::optional<CatalogDoc>{};
        uint64_t version = LNFS_TRY(core::peek_catalog_version(*text));
        return std::optional<CatalogDoc>{CatalogDoc{version, std::move(*text)}};
    }

    Result<uint64_t> write_catalog(uint64_t expected, std::string_view text) override {
        uint64_t version = LNFS_TRY(core::peek_catalog_version(text));
        if (version != expected + 1) return Err(errno_from(EINVAL));
        LNFS_TRY(ensure_layout());
        auto guard = LNFS_TRY(lock("catalog"));
        // a corrupt current file is EINVAL: fix it first
        auto current = LNFS_TRY(read_catalog());
        if ((current ? current->version : 0) != expected) return Err(errno_from(EAGAIN));
        if (current) {
            LNFS_TRY(ensure_dir(history_dir()));
            LNFS_TRY(core::atomic_write_file(history_path(current->version), current->text));
        }
        LNFS_TRY(core::atomic_write_file(dir_ + "/catalog.toml", text));
        prune_history();
        return version;
    }

    Result<std::vector<uint64_t>> list_catalog_history() override {
        std::vector<uint64_t> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(history_dir(), ec);
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            auto name = entry.path().filename().string();
            if (!name.ends_with(".toml") || name.find(".tmp.") != std::string::npos || !entry.is_regular_file(ec))
                continue;
            std::string_view stem(name);
            stem.remove_suffix(sizeof(".toml") - 1);
            if (auto version = parse_u64(stem); version && std::to_string(*version) == stem) out.push_back(*version);
        }
        if (ec) return Err(errno_from(ec.value()));
        std::sort(out.begin(), out.end());
        return out;
    }

    Result<CatalogDoc> read_catalog_history(uint64_t version) override {
        auto text = LNFS_TRY(core::read_file_if_exists(history_path(version)));
        if (!text) return Err(errno_from(ENOENT));
        return CatalogDoc{LNFS_TRY(core::peek_catalog_version(*text)), std::move(*text)};
    }

    Result<void> put_catalog_applied(const CatalogApplied& applied) override {
        if (!valid_applied_node(applied.node) || applied.digest.empty() ||
            applied.digest.find(' ') != std::string::npos || applied.status.empty() ||
            applied.status.find('\n') != std::string::npos)
            return Err(errno_from(EINVAL));
        LNFS_TRY(ensure_layout());
        return core::atomic_write_file(dir_ + "/catalog." + applied.node,
                                       std::to_string(applied.version) + " " + applied.digest + " " +
                                           std::to_string(applied.applied_at_ms) + " " + applied.status + "\n");
    }

    Result<std::vector<CatalogApplied>> list_catalog_applied() override {
        std::vector<CatalogApplied> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir_, ec);
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            auto name = entry.path().filename().string();
            if (!name.starts_with("catalog.") || name.find(".tmp.") != std::string::npos || !entry.is_regular_file(ec))
                continue;
            std::string node = name.substr(sizeof("catalog.") - 1);
            // catalog.toml / catalog.lock
            if (!valid_applied_node(node)) continue;
            auto text = core::read_file_if_exists(entry.path().string());
            if (!text || !*text) continue;
            out.push_back(LNFS_TRY(parse_catalog_applied(std::move(node), **text)));
        }
        if (ec) return Err(errno_from(ec.value()));
        std::sort(out.begin(), out.end(),
                  [](const CatalogApplied& a, const CatalogApplied& b) { return a.node < b.node; });
        return out;
    }

 private:
    std::string history_dir() const { return dir_ + "/catalog.history"; }
    std::string history_path(uint64_t version) const { return history_dir() + "/" + std::to_string(version) + ".toml"; }
    // Best effort, under catalog.lock: drop the oldest beyond kCatalogHistoryKeep.
    void prune_history() {
        auto versions = list_catalog_history();
        if (!versions || versions->size() <= kCatalogHistoryKeep) return;
        for (size_t i = 0; i + kCatalogHistoryKeep < versions->size(); ++i)
            (void)::unlink(history_path((*versions)[i]).c_str());
    }
    // catalog.<node> must not collide with the catalog's own files.
    static bool valid_applied_node(std::string_view node) {
        return !node.empty() && node != "toml" && node != "lock" && node != "history" &&
               node.find('/') == std::string_view::npos && node.find(' ') == std::string_view::npos;
    }
    // "<version> <digest> <applied_at_ms> <status>": the status is the rest of the line.
    static Result<CatalogApplied> parse_catalog_applied(std::string node, std::string_view text) {
        std::string_view line = trim(text);
        size_t a = line.find(' ');
        size_t b = a == std::string_view::npos ? a : line.find(' ', a + 1);
        size_t c = b == std::string_view::npos ? b : line.find(' ', b + 1);
        if (c == std::string_view::npos) return Err(errno_from(EINVAL));
        CatalogApplied rec;
        rec.node = std::move(node);
        rec.version = LNFS_TRY(parse_u64(line.substr(0, a)));
        rec.digest = std::string(line.substr(a + 1, b - a - 1));
        rec.applied_at_ms = static_cast<int64_t>(LNFS_TRY(parse_u64(line.substr(b + 1, c - b - 1))));
        rec.status = std::string(line.substr(c + 1));
        if (rec.status.empty()) return Err(errno_from(EINVAL));
        return rec;
    }

    // Releases the O_EXCL lock file when the owning operation returns.
    struct LockGuard {
        std::string path;
        std::unique_lock<std::mutex> local;
        LockGuard(std::string p, std::unique_lock<std::mutex> l) : path(std::move(p)), local(std::move(l)) {}
        LockGuard(LockGuard&& o) noexcept : path(std::exchange(o.path, std::string())), local(std::move(o.local)) {}
        LockGuard& operator=(LockGuard&&) = delete;
        ~LockGuard() {
            if (!path.empty()) ::unlink(path.c_str());
        }
    };

    Result<void> ensure_layout() {
        std::error_code ec;
        std::filesystem::create_directories(dir_ + "/clients", ec);
        if (ec) return Err(errno_from(ec.value()));
        return {};
    }

    static Result<void> ensure_dir(const std::string& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) return Err(errno_from(ec.value()));
        return {};
    }

    std::string fs_dir(uint32_t fsid) const { return dir_ + "/fs/" + std::to_string(fsid); }

    static bool expired(const FenceRecord& rec) { return now_ms() > rec.expires_at_ms + kFenceSkewTolerance.count(); }

    // A counter file ("<n>\n"): 0 while absent.  bump_counter runs under the caller's lock.
    static Result<uint64_t> read_counter(const std::string& path) {
        auto text = LNFS_TRY(core::read_file_if_exists(path));
        if (!text) return uint64_t{0};
        return parse_u64(trim(*text));
    }
    static Result<uint64_t> bump_counter(const std::string& path) {
        uint64_t n = LNFS_TRY(read_counter(path)) + 1;
        LNFS_TRY(core::atomic_write_file(path, std::to_string(n) + "\n"));
        return n;
    }

    // Reclaim lists (the global one and the per-fsid ones share the file format).
    static Result<std::vector<std::string>> list_clients_in(const std::string& dir) {
        std::vector<std::string> out;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        // nothing persisted yet
        if (ec == std::errc::no_such_file_or_directory) return out;
        if (ec) return Err(errno_from(ec.value()));
        for (const auto& entry : it) {
            if (!entry.is_regular_file(ec)) continue;
            auto name = entry.path().filename().string();
            // in-flight atomic write
            if (name.find(".tmp.") != std::string::npos) continue;
            auto text = core::read_file_if_exists(entry.path().string());
            if (text && *text && !(*text)->empty()) out.push_back(std::move(**text));
        }
        if (ec) return Err(errno_from(ec.value()));
        return out;
    }
    static Result<void> put_client_in(const std::string& dir, std::string_view owner_id) {
        return core::atomic_write_file(dir + "/" + client_file_name(owner_id), owner_id);
    }
    static Result<void> erase_client_in(const std::string& dir, std::string_view owner_id) {
        if (::unlink((dir + "/" + client_file_name(owner_id)).c_str()) < 0 && errno != ENOENT)
            return Err(errno_from(errno));
        return {};
    }

    // fence.<node>: "<expires_at_ms> <fsid>:<epoch>[,<fsid>:<epoch>...]\n" (no list = heartbeat).
    static const NodeFences::Hold* find_hold(const NodeFences& rec, uint32_t fsid) {
        for (const auto& hold : rec.holds)
            if (hold.fsid == fsid) return &hold;
        return nullptr;
    }
    static Result<NodeFences> parse_node_fences(std::string node, std::string_view text) {
        NodeFences rec;
        rec.node = std::move(node);
        std::string_view line = trim(text);
        size_t sp = line.find(' ');
        rec.expires_at_ms = static_cast<int64_t>(LNFS_TRY(parse_u64(line.substr(0, sp))));
        if (sp == std::string_view::npos) return rec;
        std::string_view list = line.substr(sp + 1);
        while (!list.empty()) {
            size_t comma = list.find(',');
            std::string_view item = list.substr(0, comma);
            size_t colon = item.find(':');
            if (colon == std::string_view::npos) return Err(errno_from(EINVAL));
            uint64_t fsid = LNFS_TRY(parse_u64(item.substr(0, colon)));
            if (fsid == 0 || fsid > UINT32_MAX) return Err(errno_from(EINVAL));
            rec.holds.push_back({static_cast<uint32_t>(fsid), LNFS_TRY(parse_u64(item.substr(colon + 1)))});
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        std::sort(rec.holds.begin(), rec.holds.end(), [](const auto& a, const auto& b) { return a.fsid < b.fsid; });
        return rec;
    }
    Result<std::optional<NodeFences>> read_node_fences(std::string_view node) {
        auto text = LNFS_TRY(core::read_file_if_exists(dir_ + "/fence." + std::string(node)));
        if (!text) return std::optional<NodeFences>{};
        return std::optional<NodeFences>{LNFS_TRY(parse_node_fences(std::string(node), *text))};
    }
    Result<void> write_node_fences(const NodeFences& rec) {
        std::string text = std::to_string(rec.expires_at_ms);
        for (size_t i = 0; i < rec.holds.size(); ++i)
            text += (i ? "," : " ") + std::to_string(rec.holds[i].fsid) + ":" + std::to_string(rec.holds[i].epoch);
        return core::atomic_write_file(dir_ + "/fence." + rec.node, text + "\n");
    }

    // "<epoch> <expires_at_ms> <node>\n"; the node is the rest of the line so it may
    // contain spaces.
    static Result<FenceRecord> parse_fence(std::string_view text) {
        text = trim(text);
        size_t a = text.find(' ');
        if (a == std::string_view::npos) return Err(errno_from(EINVAL));
        size_t b = text.find(' ', a + 1);
        if (b == std::string_view::npos) return Err(errno_from(EINVAL));
        FenceRecord rec;
        rec.epoch = LNFS_TRY(parse_u64(text.substr(0, a)));
        rec.expires_at_ms = static_cast<int64_t>(LNFS_TRY(parse_u64(text.substr(a + 1, b - a - 1))));
        rec.node = std::string(text.substr(b + 1));
        if (rec.node.empty()) return Err(errno_from(EINVAL));
        return rec;
    }

    Result<void> write_fence(const FenceRecord& rec) {
        return core::atomic_write_file(dir_ + "/fence", std::to_string(rec.epoch) + " " +
                                                            std::to_string(rec.expires_at_ms) + " " + rec.node + "\n");
    }

    // Takes <name>.lock (O_CREAT|O_EXCL; content "<pid> <unix_ms>").  A lock older than
    // stale_lock_after_ belongs to a dead writer: it is unlinked (once) and the create
    // retried.  A live lock is retried for a short window (another gateway mid-write),
    // then EBUSY.  The process-local mutex keeps two threads of one process from seeing
    // each other's lock file as a foreign holder.
    Result<LockGuard> lock(const char* name) {
        std::string path = dir_ + "/" + name + ".lock";
        std::unique_lock<std::mutex> local(mu_);
        bool reclaimed = false;
        auto deadline = std::chrono::steady_clock::now() + kLockWait;
        for (;;) {
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) {
                std::string stamp = std::to_string(::getpid()) + " " + std::to_string(now_ms()) + "\n";
                (void)!::write(fd, stamp.data(), stamp.size());
                ::close(fd);
                return LockGuard{std::move(path), std::move(local)};
            }
            if (errno != EEXIST) return Err(errno_from(errno));
            if (!reclaimed && lock_is_stale(path)) {
                LNFS_WARN("cluster store: reclaiming stale lock {}", path);
                (void)::unlink(path.c_str());
                reclaimed = true;
                // the retry after a reclaim never counts against the wait budget
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) return Err(errno_from(EBUSY));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool lock_is_stale(const std::string& path) const {
        int64_t taken_ms = 0;
        if (auto text = core::read_file_if_exists(path); text && *text) {
            std::string_view line = trim(**text);
            size_t sp = line.find(' ');
            if (sp != std::string_view::npos)
                if (auto ms = parse_u64(line.substr(sp + 1))) taken_ms = static_cast<int64_t>(*ms);
        }
        // unreadable or empty: fall back to the file's mtime
        if (taken_ms == 0) {
            struct stat st{};
            // vanished: not stale, retry
            if (::stat(path.c_str(), &st) < 0) return errno == ENOENT;
            taken_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
        }
        return now_ms() - taken_ms > stale_lock_after_.count();
    }

    std::string dir_;
    std::chrono::milliseconds stale_lock_after_;
    std::mutex mu_;
};

}  // namespace

std::unique_ptr<ClusterStore> make_posix_cluster_store(std::string shared_dir,
                                                       std::chrono::milliseconds stale_lock_after) {
    while (shared_dir.size() > 1 && shared_dir.back() == '/') shared_dir.pop_back();
    return std::make_unique<PosixClusterStore>(std::move(shared_dir), stale_lock_after);
}

}  // namespace lnfs::server
