#include "server/ctl.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "backend/cephfs/cephfs.hpp"
#include "backend/gluster/gluster.hpp"
#include "backend/local/local.hpp"
#include "obs/errlog.hpp"
#include "obs/metrics.hpp"
#include "rpc/drc.hpp"
#include "runtime/offload_pool.hpp"
#include "server/cluster_controller.hpp"
#include "state/state_mgr.hpp"
#include "transport/connection.hpp"
#include "runtime/io.hpp"
#include "util/log.hpp"

#ifndef LIGHTNFS_VERSION
#define LIGHTNFS_VERSION "dev"
#endif

namespace lnfs::server {

using namespace lnfs::rt;

namespace {

Task<void> send_all(int fd, std::string_view text) {
  size_t off = 0;
  while (off < text.size()) {
    iovec iov{const_cast<char*>(text.data()) + off, text.size() - off};
    int n = co_await uring_sendv(fd, &iov, 1);
    if (n <= 0) co_return;
    off += static_cast<size_t>(n);
  }
}

// Command line -> tokens; a `--json` token anywhere selects the JSON rendering
// (plan doc 10 §4.2 — scripts stop grepping free text).
struct Cmd {
  std::vector<std::string> args;
  bool json = false;
  const std::string& name() const {
    static const std::string empty;
    return args.empty() ? empty : args[0];
  }
  std::string_view arg(size_t i) const {
    return i < args.size() ? std::string_view(args[i]) : std::string_view();
  }
};

Cmd parse_command(std::string_view line) {
  Cmd out;
  std::istringstream in{std::string(line)};
  std::string tok;
  while (in >> tok) {
    if (tok == "--json") out.json = true;
    else out.args.push_back(std::move(tok));
  }
  return out;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
          out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
        else out += c;
    }
  }
  return out;
}

const char* kHelp =
    "unknown command; available: ping|version|status|metrics|dump-errors|drc [flush]|"
    "fdcache [flush]|clear-poison|state|expire-client <clientid>|conns|kill-conn <id>|"
    "loglevel <debug|info|warn|error>|reload|drain|grace-end|"
    "cluster <status|exports [<node>]|takeover [<fsid>] [--force]|standby [<fsid>]|"
    "migrate <fsid> <node>>  (append --json for JSON output)\n";

}  // namespace

CtlDeps CtlDeps::with_plane(const DataPlane* plane) {
  CtlDeps deps;
  deps.plane = std::make_shared<DataPlaneSlot>(plane);
  return deps;
}

PlaneRef::~PlaneRef() {
  if (slot_) slot_->pins_.fetch_sub(1, std::memory_order_acq_rel);
}

PlaneRef DataPlaneSlot::acquire() {
  // Pin first, then read: a detach that stores null before our load sees our pin and
  // waits; one that stores null after our load also waits, because the pin is up.
  pins_.fetch_add(1, std::memory_order_acq_rel);
  const DataPlane* plane = plane_.load(std::memory_order_acquire);
  if (!plane) {
    pins_.fetch_sub(1, std::memory_order_acq_rel);
    return {};
  }
  return PlaneRef(this, plane);
}

bool DataPlaneSlot::detach(std::chrono::milliseconds timeout) {
  plane_.store(nullptr, std::memory_order_release);
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (pins_.load(std::memory_order_acquire) > 0) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

namespace {
const char* not_active(bool json) {
  return json ? "{\"error\":\"not active\"}\n" : "not active\n";
}

const char* cluster_not_enabled(bool json) {
  return json ? "{\"error\":\"not enabled\"}\n" : "cluster: not enabled\n";
}

const char* cluster_usage(bool json) {
  return json ? "{\"error\":\"bad subcommand\"}\n"
              : "cluster: expected status|takeover [--force]|standby\n";
}

const char* cluster_fs_usage(bool json) {
  return json ? "{\"error\":\"bad subcommand\"}\n"
              : "cluster: expected status|exports [<node>]|takeover <fsid> [--force]|standby "
                "<fsid>|migrate <fsid> <node>\n";
}

// The fsid positional of `cluster takeover|standby <fsid>`: a decimal number.
std::optional<uint32_t> parse_fsid(std::string_view text) {
  if (text.empty() || text.size() > 10) return std::nullopt;
  uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10 + static_cast<uint64_t>(ch - '0');
  }
  if (value == 0 || value > 0xffffffffULL) return std::nullopt;
  return static_cast<uint32_t>(value);
}

std::string cluster_error(bool json, std::string_view text) {
  return json ? std::format("{{\"error\":\"{}\"}}\n", json_escape(text))
              : std::format("cluster: {}\n", text);
}

// `cluster status` (plan 10 C3): the controller's snapshot plus the store's peer list
// (read off the reactor by the caller).  The fence fields describe the record last
// seen: our own while we hold it (age = time since the last renew), someone else's
// while we stand by (age = time since we read it).
std::string cluster_status(const ClusterController& cc,
                           const Result<std::vector<std::string>>& peers, bool json) {
  const auto snap = cc.snapshot();
  const auto& cfg = cc.config();
  const auto now = std::chrono::steady_clock::now();
  const int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
  std::string owner = snap.fence ? snap.fence->node : "";
  int64_t fence_epoch = snap.fence ? static_cast<int64_t>(snap.fence->epoch) : -1;
  int64_t age_ms = snap.fence ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - snap.fence_seen)
                                    .count()
                              : -1;
  int64_t expires_in_ms = snap.fence ? snap.fence->expires_at_ms - wall_ms : 0;
  if (json) {
    std::string peer_list = "null";
    if (peers) {
      peer_list = "[";
      for (size_t i = 0; i < peers->size(); ++i)
        peer_list += std::format("{}\"{}\"", i ? "," : "", json_escape((*peers)[i]));
      peer_list += "]";
    }
    std::string fence_owner = snap.fence ? std::format("\"{}\"", json_escape(owner)) : "null";
    std::string fence_epoch_s = snap.fence ? std::to_string(fence_epoch) : "null";
    std::string fence_age = snap.fence ? std::to_string(age_ms) : "null";
    std::string fence_left = snap.fence ? std::to_string(expires_in_ms) : "null";
    return std::format(
        "{{\"role\":\"{}\",\"node\":\"{}\",\"epoch\":{},\"fence_owner\":{},"
        "\"fence_epoch\":{},\"fence_age_ms\":{},\"fence_expires_in_ms\":{},"
        "\"shared_dir\":\"{}\",\"peers\":{},\"takeover\":\"{}\",\"takeovers\":{},"
        "\"fence_lost\":{},\"activation_failures\":{},\"last_activation_ms\":{}}}\n",
        role_name(snap.role), json_escape(snap.node), snap.epoch, fence_owner,
        fence_epoch_s, fence_age, fence_left, json_escape(cfg.shared_dir), peer_list,
        json_escape(cfg.takeover), snap.takeovers, snap.fence_lost,
        snap.activation_failures, snap.last_activation.count());
  }
  std::string peer_list = "?";
  if (peers) {
    peer_list.clear();
    for (const auto& p : *peers) peer_list += (peer_list.empty() ? "" : ",") + p;
  }
  return std::format(
      "role={} node={} epoch={} fence_owner={} fence_epoch={} fence_age_ms={} "
      "fence_expires_in_ms={} shared_dir={} peers={} takeover={} takeovers={} "
      "fence_lost={} activation_failures={} last_activation_ms={}\n",
      role_name(snap.role), snap.node, snap.epoch, snap.fence ? owner : "none",
      snap.fence ? std::to_string(fence_epoch) : "-",
      snap.fence ? std::to_string(age_ms) : "-",
      snap.fence ? std::to_string(expires_in_ms) : "-", cfg.shared_dir, peer_list,
      cfg.takeover, snap.takeovers, snap.fence_lost, snap.activation_failures,
      snap.last_activation.count());
}

// `cluster status` under active-active (plan 12 C4): the gateway line, then one line
// per export as this gateway sees it.  Fence age = time since the record naming the
// export was last renewed (ours or theirs), from its expiry and the ttl; grace = the
// export's own reclaim window on this gateway (0 when not served here).
std::string cluster_fs_status(const FsClusterController& fc, const DataPlane* dp,
                              const Result<std::vector<std::string>>& peers,
                              const Result<std::vector<std::string>>& alive, bool json) {
  const auto snap = fc.snapshot();
  const auto& cfg = fc.config();
  const int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
  const int64_t ttl_ms = fc.fence_ttl().count();
  auto grace_left = [&](const FsClusterController::FsState& fs) -> int64_t {
    if (fs.role != Role::kActive || !dp || !dp->state) return 0;
    return dp->state->grace_remaining_seconds(fs.fsid);
  };
  auto json_list = [](const Result<std::vector<std::string>>& names) {
    if (!names) return std::string("null");
    std::string out = "[";
    for (size_t i = 0; i < names->size(); ++i)
      out += std::format("{}\"{}\"", i ? "," : "", json_escape((*names)[i]));
    return out + "]";
  };
  auto text_list = [](const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ",") + n;
    return out.empty() ? std::string("-") : out;
  };
  if (json) {
    std::string rows;
    for (const auto& fs : snap) {
      const bool owned = fs.view.role != core::FsRole::kUnowned && !fs.view.node.empty();
      rows += std::format(
          "{}{{\"fsid\":{},\"role\":\"{}\",\"nodes\":{},\"owner\":{},\"address\":{},"
          "\"fs_epoch\":{},\"fence_age_ms\":{},\"fence_expires_in_ms\":{},"
          "\"grace_remaining_s\":{},\"takeovers\":{},\"fence_lost\":{},"
          "\"activation_failures\":{}}}",
          rows.empty() ? "" : ",", fs.fsid, FsClusterController::fs_role_label(fs),
          json_list(Result<std::vector<std::string>>(fs.nodes)),
          owned ? std::format("\"{}\"", json_escape(fs.view.node)) : "null",
          owned && !fs.view.address.empty() ? std::format("\"{}\"", json_escape(fs.view.address))
                                            : "null",
          fs.view.fs_epoch,
          fs.fence ? std::to_string(wall_ms - (fs.fence->expires_at_ms - ttl_ms)) : "null",
          fs.fence ? std::to_string(fs.fence->expires_at_ms - wall_ms) : "null", grace_left(fs),
          fs.takeovers, fs.fence_lost, fs.activation_failures);
    }
    return std::format(
        "{{\"mode\":\"active-active\",\"node\":\"{}\",\"node_epoch\":{},\"node_address\":\"{}\","
        "\"shared_dir\":\"{}\",\"peers\":{},\"peers_alive\":{},\"takeover\":\"{}\","
        "\"migrations\":{},\"exports\":[{}]}}\n",
        json_escape(fc.node()), fc.node_epoch(), json_escape(cfg.node_address),
        json_escape(cfg.shared_dir), json_list(peers), json_list(alive), json_escape(cfg.takeover),
        fc.migrations(), rows);
  }
  std::string out = std::format(
      "mode=active-active node={} node_epoch={} node_address={} shared_dir={} peers={} "
      "peers_alive={} takeover={} migrations={} exports={}\n",
      fc.node(), fc.node_epoch(), cfg.node_address, cfg.shared_dir, peers ? text_list(*peers) : "?",
      alive ? text_list(*alive) : "?", cfg.takeover, fc.migrations(), snap.size());
  for (const auto& fs : snap) {
    const bool owned = fs.view.role != core::FsRole::kUnowned && !fs.view.node.empty();
    out += std::format(
        "fsid={} role={} nodes={} owner={} address={} fs_epoch={} fence_age_ms={} "
        "fence_expires_in_ms={} grace_remaining_s={} takeovers={} fence_lost={} "
        "activation_failures={}\n",
        fs.fsid, FsClusterController::fs_role_label(fs), text_list(fs.nodes),
        owned ? fs.view.node : "none", owned && !fs.view.address.empty() ? fs.view.address : "-",
        fs.view.fs_epoch,
        fs.fence ? std::to_string(wall_ms - (fs.fence->expires_at_ms - ttl_ms)) : "-",
        fs.fence ? std::to_string(fs.fence->expires_at_ms - wall_ms) : "-", grace_left(fs),
        fs.takeovers, fs.fence_lost, fs.activation_failures);
  }
  return out;
}

// `cluster exports [<node>]` under active-active: the exports one gateway serves right
// now, as this gateway sees the store — "what does gw2 hold" for an operator, and the
// export → owner-address map v3 clients need (design 10 §10.11).  <node> defaults to
// this gateway.  A node counts as known when it is us, registered in nodes/<node>, or
// named in some export's `nodes`; an export belongs to it when the view names it as
// owner (plus our own exports still activating, which the view shows as unowned).
std::string cluster_fs_exports(const FsClusterController& fc, std::string_view node,
                               const Result<std::vector<std::pair<std::string, std::string>>>& reg,
                               const Result<std::vector<std::string>>& alive, bool json) {
  const auto snap = fc.snapshot();
  const bool self = node == fc.node();
  bool known = self;
  std::string address = self ? fc.config().node_address : std::string();
  if (reg)
    for (const auto& [name, addr] : *reg)
      if (name == node) {
        known = true;
        if (address.empty()) address = addr;
      }
  for (const auto& fs : snap)
    for (const auto& name : fs.nodes)
      if (name == node) known = true;
  if (!known)
    return cluster_error(
        json, std::format("unknown node {} (not registered, not in any export's nodes)", node));
  std::vector<const FsClusterController::FsState*> owned;
  for (const auto& fs : snap) {
    const bool named = fs.view.role != core::FsRole::kUnowned && fs.view.node == node;
    if (named || (self && fs.role == Role::kActivating)) owned.push_back(&fs);
    if (named && address.empty()) address = fs.view.address;
  }
  std::string fsids;
  for (const auto* fs : owned) fsids += std::format("{}{}", fsids.empty() ? "" : ",", fs->fsid);
  const char* alive_text = "?";
  if (alive)
    alive_text = std::find(alive->begin(), alive->end(), node) != alive->end() ? "yes" : "no";
  if (json) {
    std::string rows;
    for (const auto* fs : owned)
      rows += std::format("{}{{\"fsid\":{},\"path\":\"{}\",\"role\":\"{}\",\"fs_epoch\":{}}}",
                          rows.empty() ? "" : ",", fs->fsid, json_escape(fs->path),
                          FsClusterController::fs_role_label(*fs), fs->view.fs_epoch);
    return std::format(
        "{{\"node\":\"{}\",\"alive\":{},\"address\":{},\"fsids\":[{}],\"exports\":[{}]}}\n",
        json_escape(node), !alive ? "null" : (alive_text[0] == 'y' ? "true" : "false"),
        address.empty() ? "null" : std::format("\"{}\"", json_escape(address)), fsids, rows);
  }
  std::string out =
      std::format("node={} alive={} address={} exports={} fsids={}\n", node, alive_text,
                  address.empty() ? "-" : address, owned.size(), fsids.empty() ? "-" : fsids);
  for (const auto* fs : owned)
    out += std::format("fsid={} path={} role={} fs_epoch={}\n", fs->fsid, fs->path,
                       FsClusterController::fs_role_label(*fs), fs->view.fs_epoch);
  return out;
}

// The role label of one export for the error texts, or "unknown" for an fsid this
// gateway does not export.
std::string fs_role_of(const FsClusterController& fc, uint32_t fsid) {
  for (const auto& fs : fc.snapshot())
    if (fs.fsid == fsid) return FsClusterController::fs_role_label(fs);
  return "unknown";
}
}  // namespace

std::string CtlServer::answer(const CtlDeps& deps, std::string_view command) {
  const Cmd cmd = parse_command(command);
  const bool json = cmd.json;
  PlaneRef pin = deps.acquire_plane();
  const DataPlane* dp = pin.get();
  if (cmd.name() == "ping") return json ? "{\"ok\":true}\n" : "pong\n";
  if (cmd.name() == "version") {
    return json ? std::format("{{\"version\":\"{}\"}}\n", LIGHTNFS_VERSION)
                : std::format("lightnfs {}\n", LIGHTNFS_VERSION);
  }
  if (cmd.name() == "status") {
    auto uptime =
        deps.started.time_since_epoch().count() == 0
            ? 0
            : std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - deps.started)
                  .count();
    size_t conns = transport::ConnRegistry::instance().count();
    std::string role = deps.role         ? deps.role()
                       : deps.cluster    ? role_name(deps.cluster->role())
                       : deps.fs_cluster ? "active-active"
                       : dp              ? "active"
                                         : "standby";
    bool draining = dp && dp->draining && dp->draining->load(std::memory_order_relaxed);
    size_t exports = dp && dp->exports ? dp->exports->size() : 0;
    bool grace = dp && dp->state && dp->state->in_grace();
    int64_t grace_left = dp && dp->state ? dp->state->grace_remaining_seconds() : 0;
    if (json)
      return std::format(
          "{{\"version\":\"{}\",\"uptime_s\":{},\"role\":\"{}\",\"connections\":{},"
          "\"draining\":{},\"exports\":{},\"grace\":{},\"grace_remaining_s\":{}}}\n",
          LIGHTNFS_VERSION, uptime, json_escape(role), conns, draining, exports, grace,
          grace_left);
    return std::format(
        "version={} uptime={}s role={} connections={} draining={} exports={} grace={} "
        "grace_remaining={}s\n",
        LIGHTNFS_VERSION, uptime, role, conns, draining ? 1 : 0, exports, grace ? 1 : 0,
        grace_left);
  }
  if (cmd.name() == "metrics") return obs::prometheus_text();
  if (cmd.name() == "dump-errors")
    return json ? obs::dump_error_replies_json() : obs::dump_error_replies();
  if (cmd.name() == "loglevel") {
    auto lv = cmd.arg(1);
    if (lv == "debug") set_log_level(LogLevel::kDebug);
    else if (lv == "info") set_log_level(LogLevel::kInfo);
    else if (lv == "warn") set_log_level(LogLevel::kWarn);
    else if (lv == "error") set_log_level(LogLevel::kError);
    else return json ? "{\"error\":\"bad level\"}\n"
                     : "loglevel: expected debug|info|warn|error\n";
    LNFS_INFO("log level set to {} via ctl", lv);
    return json ? std::format("{{\"loglevel\":\"{}\"}}\n", lv)
                : std::format("log level set to {}\n", lv);
  }
  if (cmd.name() == "reload") {
    if (!deps.reload) return json ? "{\"error\":\"unavailable\"}\n" : "reload unavailable\n";
    std::string report = deps.reload();
    return json ? std::format("{{\"reloaded\":true,\"report\":\"{}\"}}\n",
                              json_escape(report))
                : report;
  }
  if (cmd.name() == "drain") {
    if (!dp) return not_active(json);
    if (!dp->drain) return json ? "{\"error\":\"unavailable\"}\n" : "drain unavailable\n";
    std::string report = dp->drain();
    return json ? std::format("{{\"draining\":true,\"report\":\"{}\"}}\n",
                              json_escape(report))
                : report;
  }
  if (cmd.name() == "grace-end") {
    if (!dp) return not_active(json);
    if (!dp->state) return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    bool was = dp->state->end_grace();
    if (json) return std::format("{{\"grace_ended\":{}}}\n", was);
    return was ? "grace period ended\n" : "not in grace\n";
  }
  if (cmd.name() == "conns") {
    auto conns = transport::ConnRegistry::instance().list();
    std::string out;
    if (json) {
      out = "[";
      for (size_t i = 0; i < conns.size(); ++i)
        out += std::format("{}{{\"id\":{},\"peer\":\"{}\",\"age_s\":{}}}",
                           i ? "," : "", conns[i].id, json_escape(conns[i].peer),
                           conns[i].age_s);
      out += "]\n";
      return out;
    }
    for (const auto& c : conns)
      out += std::format("id={} peer={} age={}s\n", c.id, c.peer, c.age_s);
    return out.empty() ? "no connections\n" : out;
  }
  if (cmd.name() == "kill-conn") {
    uint64_t id = 0;
    auto arg = cmd.arg(1);
    auto [p, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), id);
    if (ec != std::errc() || p != arg.data() + arg.size())
      return json ? "{\"error\":\"bad id\"}\n" : "kill-conn: expected a numeric id (see conns)\n";
    bool killed = transport::ConnRegistry::instance().kill(id);
    if (json) return std::format("{{\"killed\":{}}}\n", killed);
    return killed ? std::format("connection {} shut down\n", id)
                  : std::format("connection {} not found\n", id);
  }
  if (cmd.name() == "drc") {
    if (!dp) return not_active(json);
    if (!dp->drc) return json ? "{\"error\":\"drc disabled\"}\n" : "drc disabled\n";
    auto s = dp->drc->stats();
    if (json)
      return std::format(
          "{{\"inserts\":{},\"replays\":{},\"waits\":{},\"evictions\":{},"
          "\"entries\":{},\"bytes\":{}}}\n",
          s.inserts, s.replays, s.waits, s.evictions, s.entries, s.bytes);
    return std::format(
        "inserts={} replays={} waits={} evictions={} entries={} bytes={}\n", s.inserts,
        s.replays, s.waits, s.evictions, s.entries, s.bytes);
  }
  if (cmd.name() == "fdcache" && cmd.arg(1) == "flush") {
    if (!dp) return not_active(json);
    size_t total = 0;
    bool any = false;
    if (dp->exports) {
      auto set = dp->exports->snapshot();
      for (const auto& entry : set->entries) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          any = true;
          total += g->flush_fd_cache();
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          any = true;
          total += c->flush_fd_cache();
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        any = true;
        total += local->flush_fd_cache();
      }
    }
    if (json) return std::format("{{\"flushed\":{}}}\n", total);
    return any ? std::format("flushed {} cached fds\n", total) : "no local/gluster/cephfs exports\n";
  }
  if (cmd.name() == "fdcache") {
    if (!dp) return not_active(json);
    std::string out;
    if (json) out = "[";
    size_t emitted = 0;
    if (dp->exports) {
      auto set = dp->exports->snapshot();
      for (const auto& entry : set->entries) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          auto s = g->stats();
          if (json) {
            out += std::format(
                "{}{{\"export\":\"{}\",\"backend\":\"gluster\",\"hits\":{},\"misses\":{},"
                "\"upgrades\":{},\"evictions\":{},\"entries\":{},\"obj_hits\":{},"
                "\"obj_misses\":{},\"obj_entries\":{},\"jukebox\":{},\"lock_fds\":{}}}",
                emitted ? "," : "", json_escape(entry->path), s.fd_hits, s.fd_misses,
                s.fd_upgrades, s.fd_evictions, s.fd_entries, s.obj_hits, s.obj_misses,
                s.obj_entries, s.jukebox, s.lock_fds);
          } else {
            out += std::format(
                "export={} backend=gluster hits={} misses={} upgrades={} evictions={} "
                "entries={} obj_hits={} obj_misses={} obj_entries={} jukebox={} lock_fds={}\n",
                entry->path, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions,
                s.fd_entries, s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox, s.lock_fds);
          }
          ++emitted;
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          auto s = c->stats();
          if (json) {
            out += std::format(
                "{}{{\"export\":\"{}\",\"backend\":\"cephfs\",\"hits\":{},\"misses\":{},"
                "\"upgrades\":{},\"evictions\":{},\"entries\":{},\"obj_hits\":{},"
                "\"obj_misses\":{},\"obj_entries\":{},\"jukebox\":{},\"blocklisted\":{},"
                "\"lock_fds\":{}}}",
                emitted ? "," : "", json_escape(entry->path), s.fd_hits, s.fd_misses,
                s.fd_upgrades, s.fd_evictions, s.fd_entries, s.obj_hits, s.obj_misses,
                s.obj_entries, s.jukebox, s.blocklisted, s.lock_fds);
          } else {
            out += std::format(
                "export={} backend=cephfs hits={} misses={} upgrades={} evictions={} "
                "entries={} obj_hits={} obj_misses={} obj_entries={} jukebox={} "
                "blocklisted={} lock_fds={}\n",
                entry->path, s.fd_hits, s.fd_misses, s.fd_upgrades, s.fd_evictions,
                s.fd_entries, s.obj_hits, s.obj_misses, s.obj_entries, s.jukebox,
                s.blocklisted, s.lock_fds);
          }
          ++emitted;
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        auto s = local->fd_cache_stats();
        if (json) {
          out += std::format(
              "{}{{\"export\":\"{}\",\"hits\":{},\"misses\":{},\"upgrades\":{},"
              "\"evictions\":{},\"overflows\":{},\"entries\":{},\"path_hits\":{},"
              "\"path_misses\":{},\"path_entries\":{}}}",
              emitted ? "," : "", json_escape(entry->path), s.hits, s.misses,
              s.upgrades, s.evictions, s.overflows, s.entries, s.path_hits,
              s.path_misses, s.path_entries);
        } else {
          out += std::format(
              "export={} hits={} misses={} upgrades={} evictions={} overflows={} "
              "entries={} path_hits={} path_misses={} path_entries={}\n",
              entry->path, s.hits, s.misses, s.upgrades, s.evictions, s.overflows,
              s.entries, s.path_hits, s.path_misses, s.path_entries);
        }
        ++emitted;
      }
    }
    if (json) return out + "]\n";
    return out.empty() ? "no local/gluster/cephfs exports\n" : out;
  }
  if (cmd.name() == "clear-poison") {
    // Sticky fsync-EIO marks (design 06 §6.2) previously survived until restart
    // (plan doc 10 §1.5); this is the operator's way out after fixing the media.
    if (!dp) return not_active(json);
    size_t total = 0;
    bool any = false;
    if (dp->exports) {
      auto set = dp->exports->snapshot();
      for (const auto& entry : set->entries) {
        if (auto* g = dynamic_cast<backend::GlusterBackend*>(entry->backend.get())) {
          any = true;
          total += g->clear_poison();
          continue;
        }
        if (auto* c = dynamic_cast<backend::CephBackend*>(entry->backend.get())) {
          any = true;
          total += c->clear_poison();
          continue;
        }
        auto* local = dynamic_cast<backend::LocalBackend*>(entry->backend.get());
        if (!local) continue;
        any = true;
        total += local->clear_poison();
      }
    }
    if (json) return std::format("{{\"cleared\":{}}}\n", total);
    return any ? std::format("cleared {} poison marks\n", total) : "no local/gluster/cephfs exports\n";
  }
  // `cluster *` runs its store IO off the reactor in answer_async(); only the
  // single-gateway answer is available here.
  if (cmd.name() == "cluster" && !deps.cluster && !deps.fs_cluster)
    return cluster_not_enabled(json);
  return json ? "{\"error\":\"unknown command\"}\n" : kHelp;
}

rt::Task<std::string> CtlServer::answer_async(const CtlDeps& deps, std::string command) {
  const Cmd cmd = parse_command(command);
  const bool json = cmd.json;
  // The pin keeps the plane alive across the awaits below: a detach in progress waits
  // for it to drop before the data plane is torn down (plan 10 C1).
  PlaneRef pin = deps.acquire_plane();
  const DataPlane* dp = pin.get();
  if (cmd.name() == "state") {
    if (!dp) co_return not_active(json);
    if (!dp->state) co_return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    auto s = dp->state->stats();
    if (json) {
      // Counters only: the table dumps stay a human-format text view.
      co_return std::format(
          "{{\"clients\":{},\"sessions\":{},\"opens\":{},\"files\":{},\"courtesy\":{},"
          "\"grace\":{},\"grace_remaining_s\":{},\"lease_expirations\":{},"
          "\"reclaim_conflict\":{},\"reclaim_timeout\":{},\"reclaim_forced\":{},"
          "\"share_denied\":{},\"open_merges\":{},\"lock_states\":{},"
          "\"lock_segments\":{},\"lock_owners\":{},\"lock_denied\":{}}}\n",
          s.clients, s.sessions, s.opens, s.files, s.courtesy, s.grace,
          s.grace_remaining, s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout,
          s.reclaim_forced, s.share_denied, s.open_merges, s.lock_states,
          s.lock_segments, s.lock_owners, s.lock_denied);
    }
    std::string out = std::format(
        "clients={} sessions={} opens={} files={} courtesy={} grace={} grace_remaining={}s "
        "lease_expirations={} reclaim_conflict={} reclaim_timeout={} reclaim_forced={} "
        "share_denied={} open_merges={} lock_states={} lock_segments={} lock_owners={} "
        "lock_denied={} delegs={} deleg_grants={} deleg_recalls={} deleg_returns={} "
        "deleg_revokes={} cb_lock_notifies={}\n",
        s.clients, s.sessions, s.opens, s.files, s.courtesy, s.grace ? 1 : 0,
        s.grace_remaining, s.lease_expirations, s.reclaim_conflict, s.reclaim_timeout,
        s.reclaim_forced, s.share_denied, s.open_merges, s.lock_states, s.lock_segments,
        s.lock_owners, s.lock_denied, s.delegs, s.deleg_grants, s.deleg_recalls,
        s.deleg_returns, s.deleg_revokes, s.cb_lock_notifies);
    out += co_await dp->state->dump();
    co_return out;
  }
  if (cmd.name() == "expire-client") {
    if (!dp) co_return not_active(json);
    if (!dp->state) co_return json ? "{\"error\":\"v4 disabled\"}\n" : "v4 disabled\n";
    uint64_t id = 0;
    try {
      id = std::stoull(std::string(cmd.arg(1)), nullptr, 0);
    } catch (...) {
      co_return json ? "{\"error\":\"bad clientid\"}\n" : "expire-client: bad clientid\n";
    }
    uint32_t status = co_await dp->state->expire_client(id);
    if (json) co_return std::format("{{\"clientid\":\"{:#x}\",\"status\":{}}}\n", id, status);
    co_return status == 0 ? std::format("client {:#x} reclaimed\n", id)
                          : std::format("client {:#x}: nfs4 status {}\n", id, status);
  }
  if (cmd.name() == "drc" && cmd.arg(1) == "flush") {
    if (!dp) co_return not_active(json);
    if (!dp->drc) co_return json ? "{\"error\":\"drc disabled\"}\n" : "drc disabled\n";
    size_t dropped = co_await dp->drc->flush();
    if (json) co_return std::format("{{\"flushed\":{}}}\n", dropped);
    co_return std::format("flushed {} drc entries\n", dropped);
  }
  if (cmd.name() == "cluster" && deps.cluster) {
    // The controller's store calls block on the shared filesystem (plan 10 A2): run
    // them on the offload pool, never on this reactor.
    ClusterController& cc = *deps.cluster;
    const auto sub = cmd.arg(1);
    if (sub == "status") {
      auto peers = co_await rt::offload([&cc] { return cc.peers(); });
      co_return cluster_status(cc, peers, json);
    }
    if (sub == "takeover") {
      bool force = false;
      for (size_t i = 2; i < cmd.args.size(); ++i) force |= cmd.args[i] == "--force";
      auto took = co_await rt::offload([&cc, force] { return cc.request_takeover(force); });
      if (took) {
        auto snap = cc.snapshot();
        if (json)
          co_return std::format("{{\"takeover\":true,\"forced\":{},\"epoch\":{},\"role\":\"{}\"}}\n",
                                force, snap.epoch, role_name(snap.role));
        co_return std::format("takeover started: node={} epoch={} role={}{}\n", snap.node,
                              snap.epoch, role_name(snap.role), force ? " (forced)" : "");
      }
      auto snap = cc.snapshot();
      if (snap.role != Role::kStandby)
        co_return cluster_error(json,
                                std::format("not standby (role={})", role_name(snap.role)));
      if (took.error() == errno_from(EBUSY))
        co_return cluster_error(
            json, std::format("fence held by {} (retry with --force to take it)",
                              snap.fence ? snap.fence->node : "another node"));
      co_return cluster_error(json,
                              std::format("takeover failed: {}", errno_name(took.error())));
    }
    if (sub == "standby") {
      auto asked = co_await rt::offload([&cc] { return cc.request_standby(); });
      if (asked) {
        if (json) co_return "{\"standby\":true}\n";
        co_return "standby requested: draining\n";
      }
      co_return cluster_error(
          json, std::format("not active (role={})", role_name(cc.snapshot().role)));
    }
    co_return cluster_usage(json);
  }
  if (cmd.name() == "cluster" && deps.fs_cluster) {
    // Active-active (plan 12 C4): the same shape per export.  Store IO off the reactor.
    FsClusterController& fc = *deps.fs_cluster;
    const auto sub = cmd.arg(1);
    if (sub == "status") {
      auto peers = co_await rt::offload([&fc] { return fc.peers(); });
      auto alive = co_await rt::offload([&fc] { return fc.alive_peers(); });
      co_return cluster_fs_status(fc, dp, peers, alive, json);
    }
    if (sub == "exports") {
      // `[<node>]`: the first positional after the subcommand, default this gateway.
      std::string node = fc.node();
      for (size_t i = 2; i < cmd.args.size(); ++i)
        if (!cmd.args[i].starts_with("--")) {
          node = cmd.args[i];
          break;
        }
      auto reg = co_await rt::offload([&fc] { return fc.registry(); });
      auto alive = co_await rt::offload([&fc] { return fc.alive_peers(); });
      co_return cluster_fs_exports(fc, node, reg, alive, json);
    }
    if (sub == "migrate") {
      // `<fsid> <node>`: the export we serve and the live gateway to hand it to.
      std::optional<uint32_t> fsid;
      std::string target;
      for (size_t i = 2; i < cmd.args.size(); ++i) {
        if (cmd.args[i].starts_with("--")) continue;
        if (!fsid)
          fsid = parse_fsid(cmd.args[i]);
        else if (target.empty())
          target = cmd.args[i];
      }
      if (!fsid || target.empty()) co_return cluster_error(json, "fsid and node required");
      auto moved =
          co_await rt::offload([&fc, fsid, &target] { return fc.request_migrate(*fsid, target); });
      if (moved) {
        if (json)
          co_return std::format("{{\"migrate\":true,\"fsid\":{},\"to\":\"{}\"}}\n", *fsid,
                                json_escape(target));
        co_return std::format("migrate started: fsid={} from={} to={}\n", *fsid, fc.node(), target);
      }
      std::string role = fs_role_of(fc, *fsid);
      if (role == "unknown") co_return cluster_error(json, std::format("unknown fsid {}", *fsid));
      if (moved.error() == errno_from(EINVAL))
        co_return cluster_error(json, std::format("cannot migrate fsid {} to {}: that is this "
                                                  "node",
                                                  *fsid, target));
      if (moved.error() == errno_from(EPERM)) {
        std::string owner = "nobody";
        for (const auto& fs : fc.snapshot())
          if (fs.fsid == *fsid && !fs.view.node.empty() && fs.view.role != core::FsRole::kUnowned)
            owner = fs.view.node;
        co_return cluster_error(
            json, std::format("fsid {} not active here (role={}, owner={})", *fsid, role, owner));
      }
      if (moved.error() == errno_from(EHOSTDOWN))
        co_return cluster_error(
            json,
            std::format("node {} is not a live gateway (no registration or heartbeat)", target));
      co_return cluster_error(json, std::format("migrate failed: {}", errno_name(moved.error())));
    }
    if (sub == "takeover" || sub == "standby") {
      // `<fsid>` is the first positional after the subcommand; flags may follow it.
      std::optional<uint32_t> fsid;
      bool force = false;
      for (size_t i = 2; i < cmd.args.size(); ++i) {
        if (cmd.args[i] == "--force")
          force = true;
        else if (!fsid)
          fsid = parse_fsid(cmd.args[i]);
      }
      if (!fsid) co_return cluster_error(json, "fsid required");
      if (sub == "takeover") {
        auto took =
            co_await rt::offload([&fc, fsid, force] { return fc.request_takeover(*fsid, force); });
        if (took) {
          std::string role = fs_role_of(fc, *fsid);
          uint64_t fs_epoch = 0;
          for (const auto& fs : fc.snapshot())
            if (fs.fsid == *fsid) fs_epoch = fs.fs_epoch;
          if (json)
            co_return std::format(
                "{{\"takeover\":true,\"fsid\":{},\"forced\":{},\"fs_epoch\":{},\"role\":\"{}\"}}\n",
                *fsid, force, fs_epoch, role);
          co_return std::format("takeover started: fsid={} node={} fs_epoch={} role={}{}\n", *fsid,
                                fc.node(), fs_epoch, role, force ? " (forced)" : "");
        }
        if (took.error() == errno_from(EINVAL))
          co_return cluster_error(json, std::format("unknown fsid {}", *fsid));
        std::string role = fs_role_of(fc, *fsid);
        if (fc.role_of(*fsid) != Role::kStandby)
          co_return cluster_error(json, std::format("fsid {} not remote (role={})", *fsid, role));
        if (took.error() == errno_from(EBUSY)) {
          std::string holder = "another node";
          for (const auto& fs : fc.snapshot())
            if (fs.fsid == *fsid && fs.fence) holder = fs.fence->node;
          co_return cluster_error(
              json, std::format("fsid {} fence held by {} (retry with --force to take it)", *fsid,
                                holder));
        }
        co_return cluster_error(json, std::format("takeover failed: {}", errno_name(took.error())));
      }
      auto asked = co_await rt::offload([&fc, fsid] { return fc.request_release(*fsid); });
      if (asked) {
        if (json) co_return std::format("{{\"standby\":true,\"fsid\":{}}}\n", *fsid);
        co_return std::format("standby requested: fsid={} draining\n", *fsid);
      }
      std::string role = fs_role_of(fc, *fsid);
      if (role == "unknown") co_return cluster_error(json, std::format("unknown fsid {}", *fsid));
      co_return cluster_error(json, std::format("fsid {} not active (role={})", *fsid, role));
    }
    co_return cluster_fs_usage(json);
  }
  co_return answer(deps, command);
}

Result<std::unique_ptr<CtlServer>> CtlServer::create(const std::string& socket_path,
                                                     CtlDeps deps) {
  sockaddr_un addr{};
  if (socket_path.size() >= sizeof(addr.sun_path)) return Err(errno_from(ENAMETOOLONG));
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  ::unlink(socket_path.c_str());  // stale socket from a previous run
  addr.sun_family = AF_UNIX;
  socket_path.copy(addr.sun_path, socket_path.size());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, 16) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  // Owner-only socket permissions independent of umask (plan doc 10 §1.8); the tiny
  // bind→chmod window is covered by the SO_PEERCRED gate in serve().
  (void)::chmod(socket_path.c_str(), 0600);
  return std::unique_ptr<CtlServer>(new CtlServer(fd, socket_path, deps));
}

CtlServer::~CtlServer() {
  if (fd_ >= 0) ::close(fd_);
  if (!path_.empty()) ::unlink(path_.c_str());
}

void CtlServer::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load())
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
}

rt::Task<void> CtlServer::serve(int cfd) {
  // Only root or the server's own user may issue ctl commands — expire-client is
  // destructive, and the socket path permissions alone depend on the filesystem
  // (plan doc 10 §1.8).
  ucred peer{};
  socklen_t plen = sizeof peer;
  if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &peer, &plen) != 0 ||
      (peer.uid != 0 && peer.uid != ::geteuid())) {
    co_await send_all(cfd, "permission denied\n");
    co_await uring_close(cfd);
    co_return;
  }
  // One command per connection, terminated by newline or EOF (lightnfs-ctl shuts down
  // its write side): loop so a fragmented send is not silently truncated.
  std::string line;
  std::array<std::byte, 256> buf{};
  size_t received = 0;
  while (line.size() < 4096 && line.find('\n') == std::string::npos) {
    int n = co_await uring_recv(cfd, buf);
    if (n <= 0) break;
    received += static_cast<size_t>(n);
    line.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
  }
  if (received > 0) {
    if (auto nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    co_await send_all(cfd, co_await answer_async(deps_, std::move(line)));
  }
  co_await uring_close(cfd);
}

void CtlServer::start(rt::Reactor& reactor) {
  started_ = true;
  spawn(run(), reactor);
}

void CtlServer::wait_stopped() {
  if (started_) exited_future_.wait();
}

rt::Task<void> CtlServer::run() {
  struct Exit {  // signals wait_stopped() however the loop ends
    std::promise<void>* p;
    ~Exit() { p->set_value(); }
  } exit_guard{&exited_};
  run_reactor_.store(&current_reactor());
  auto token = stop_.token();
  for (;;) {
    int cfd = co_await uring_accept(fd_, nullptr, nullptr);
    if (token.cancel_requested()) {
      if (cfd >= 0) ::close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) continue;
    spawn(serve(cfd), current_reactor());
  }
}

Result<std::unique_ptr<MetricsHttp>> MetricsHttp::create(uint16_t port,
                                                         const std::string& bind_addr,
                                                         std::vector<core::Cidr> allow) {
  // Bind the configured address instead of in6addr_any (plan doc 10 §1.8): the config
  // default is loopback, so exposing metrics beyond the host is an explicit choice.
  sockaddr_storage ss{};
  socklen_t slen = 0;
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
  auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
  if (inet_pton(AF_INET6, bind_addr.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    slen = sizeof(*v6);
  } else if (inet_pton(AF_INET, bind_addr.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    slen = sizeof(*v4);
  } else {
    return Err(errno_from(EINVAL));
  }
  int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Err(errno_from(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (ss.ss_family == AF_INET6) {
    int zero = 0;  // "::" keeps serving mapped v4 peers as before
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), slen) < 0 || ::listen(fd, 64) < 0) {
    int e = errno;
    ::close(fd);
    return Err(errno_from(e));
  }
  socklen_t alen = slen;
  getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &alen);
  uint16_t bound = ntohs(ss.ss_family == AF_INET6 ? v6->sin6_port : v4->sin_port);
  return std::unique_ptr<MetricsHttp>(new MetricsHttp(fd, bound, std::move(allow)));
}

MetricsHttp::~MetricsHttp() {
  if (fd_ >= 0) ::close(fd_);
}

void MetricsHttp::request_stop() {
  stop_.request();
  if (Reactor* r = run_reactor_.load())
    spawn([](int fd) -> Task<void> { co_await uring_cancel_fd(fd); }(fd_), *r);
}

bool MetricsHttp::allowed(const sockaddr_storage& peer) const {
  if (allow_.empty()) return true;
  return std::any_of(allow_.begin(), allow_.end(),
                     [&](const core::Cidr& c) { return c.contains(peer); });
}

rt::Task<void> MetricsHttp::serve(int cfd) {
  // Request line + headers, contents ignored.  A half-open peer used to park this
  // coroutine forever (plan doc 10 §1.8): bound the read.  The buffer is shared with
  // the detached recv so a timed-out read can never scribble on a dead frame.
  auto buf = std::make_shared<std::array<std::byte, 1024>>();
  auto got = co_await rt::with_timeout(
      [](int fd, std::shared_ptr<std::array<std::byte, 1024>> b) -> rt::Task<int> {
        co_return co_await uring_recv(fd, std::span<std::byte>(b->data(), b->size()));
      }(cfd, buf),
      std::chrono::seconds(5));
  if (!got) {
    co_await uring_cancel_fd(cfd);  // release the parked recv before the fd goes away
    co_await uring_close(cfd);
    co_return;
  }
  std::string body = obs::prometheus_text();
  std::string response = std::format(
      "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
      "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
      body.size(), body);
  co_await send_all(cfd, response);
  co_await uring_close(cfd);
}

void MetricsHttp::start(rt::Reactor& reactor) {
  started_ = true;
  spawn(run(), reactor);
}

void MetricsHttp::wait_stopped() {
  if (started_) exited_future_.wait();
}

rt::Task<void> MetricsHttp::run() {
  struct Exit {
    std::promise<void>* p;
    ~Exit() { p->set_value(); }
  } exit_guard{&exited_};
  run_reactor_.store(&current_reactor());
  auto token = stop_.token();
  for (;;) {
    sockaddr_storage peer{};
    socklen_t plen = sizeof peer;
    int cfd = co_await uring_accept(fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (token.cancel_requested()) {
      if (cfd >= 0) ::close(cfd);
      break;
    }
    if (cfd == -EINTR || cfd == -ECANCELED) continue;
    if (cfd < 0) continue;
    if (!allowed(peer)) {  // CIDR allowlist (plan doc 10 §1.8)
      ::close(cfd);
      continue;
    }
    spawn(serve(cfd), current_reactor());
  }
}

}  // namespace lnfs::server
