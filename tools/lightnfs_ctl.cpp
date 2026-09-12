// lightnfs-ctl (design 08 §8.6): admin CLI over the server's unix ctl socket, plus
// the three-layer benchmarks (design 02 §2.8) as a local subcommand family that spins
// up its own in-process stack and never touches the socket.
//
// Command tree (ccmd, third_party/ccmd):
//   lightnfs-ctl <ping|metrics|dump-errors|drc|fdcache|clear-poison|state> [--socket=PATH]
//   lightnfs-ctl expire-client <clientid> [--socket=PATH]
//   lightnfs-ctl cluster <status|exports [<node>]|takeover [<fsid>] [--force]|standby [<fsid>]|
//                        migrate <fsid> <node>> [--socket=PATH]
//   lightnfs-ctl cluster catalog <show|status|history|diff [v1] [v2]|import <file> [--dry-run]
//                        [--comment=TEXT]|rollback <version> [--comment=TEXT]|apply> [--socket=PATH]
//   lightnfs-ctl cluster export <list|add …|set <fsid> …|remove <fsid> [--force]> [--socket=PATH]
//                (forwarded verbatim: the server parses the export flags, plan 12 D2)
//   lightnfs-ctl bench <echo|nullrpc|fullpath> [args...]
//
// Socket resolution: --socket, else $LIGHTNFS_CTL, else /tmp/lightnfs-state/ctl.sock.
// Root options do not propagate to subcommands in ccmd, so normalize_argv moves
// --socket/--json (wherever they were given) behind the positionals, where the leaf
// that owns the flag sees them.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <ccmd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tools/bench/bench_main.hpp"

namespace {

using Cmd = std::shared_ptr<ccmd::command>;

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

std::string default_socket() {
    const char* env = std::getenv("LIGHTNFS_CTL");
    return env ? env : "/tmp/lightnfs-state/ctl.sock";
}

// Root options do not propagate down in ccmd, so --socket/--json are registered per leaf.
void add_socket_flag(const Cmd& cmd) {
    cmd->varp<std::string>("socket", "s", default_socket(), "path to the server ctl socket ($LIGHTNFS_CTL overrides)");
    cmd->varp<bool>("json", "j", false, "machine-readable JSON output");
}

// Sends one text line over the ctl socket and streams the reply to stdout.
int send_ctl(const std::string& path, const std::string& line) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "socket path too long\n");
        return 2;
    }
    path.copy(addr.sun_path, path.size());
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "cannot connect to %s: %s\n", path.c_str(), strerror(errno));
        if (fd >= 0) ::close(fd);
        return 1;
    }
    if (::write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
        std::fprintf(stderr, "write failed\n");
        ::close(fd);
        return 1;
    }
    ::shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, static_cast<size_t>(n), stdout);
    ::close(fd);
    return 0;
}

// Generic socket leaf: the wire line is the command name plus its positionals — the
// same protocol the server's CtlServer already speaks.
void run_socket_cmd(const Cmd& c) {
    std::string line = c->name();
    for (const auto& a : c->args()) line += " " + a;
    if (c->var<bool>("json")) line += " --json";
    line += '\n';
    g_exit = send_ctl(c->var<std::string>("socket"), line);
}

Cmd make_socket_leaf(const char* name, const char* example, const char* usage, const char* help_long,
                     const char* help_short) {
    auto cmd = std::make_shared<ccmd::command>(name, example, usage, help_long, help_short, run_socket_cmd);
    add_socket_flag(cmd);
    return cmd;
}

// The wire protocol splits on whitespace outside double quotes (plan 12 D1): an
// argument with whitespace, a quote or a backslash goes quoted, `"` and `\` escaped.
std::string wire_arg(const std::string& a) {
    bool plain = !a.empty();
    for (char c : a)
        if (c == ' ' || c == '\t' || c == '"' || c == '\\') plain = false;
    if (plain) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

// Which of the server-side flags a `cluster …` leaf forwards.
enum ClusterFlags : unsigned { kForce = 1, kDryRun = 2, kComment = 4 };

// A leaf below `cluster` (plan 10 C3 / plan 11 / plan 12): the wire line is the path
// from `cluster` down (`wire`), the positionals, and the flags the leaf registered.
Cmd make_cluster_leaf(std::string wire, const char* name, const char* example, const char* usage, const char* help_long,
                      const char* help_short, unsigned flags = 0) {
    auto run = [wire = std::move(wire), flags](const Cmd& c) {
        std::string line = wire;
        for (const auto& a : c->args()) line += " " + wire_arg(a);
        if ((flags & kForce) && c->var<bool>("force")) line += " --force";
        if ((flags & kDryRun) && c->var<bool>("dry-run")) line += " --dry-run";
        if (flags & kComment)
            if (auto comment = c->var<std::string>("comment"); !comment.empty())
                line += " --comment " + wire_arg(comment);
        if (c->var<bool>("json")) line += " --json";
        line += '\n';
        g_exit = send_ctl(c->var<std::string>("socket"), line);
    };
    auto cmd = std::make_shared<ccmd::command>(name, example, usage, help_long, help_short, run);
    add_socket_flag(cmd);
    if (flags & kForce) cmd->varp<bool>("force", "f", false, "take a live fence held by another node");
    if (flags & kDryRun) cmd->varp<bool>("dry-run", "n", false, "report the changes without committing");
    if (flags & kComment) cmd->varp<std::string>("comment", "c", "", "audit-trail comment for the new version");
    return cmd;
}

// An inner node of the tree (`cluster`, `cluster catalog`, `cluster export`): bare it
// prints its own help and exits 2.
Cmd make_node(const char* name, const char* example, const char* usage, const char* help_long, const char* help_short) {
    return std::make_shared<ccmd::command>(name, example, usage, help_long, help_short, [](const Cmd& c) {
        c->print_help();
        g_exit = 2;
    });
}

Cmd make_cluster_catalog() {
    auto cmd =
        make_node("catalog", "lightnfs-ctl cluster catalog import /etc/lightnfs/lightnfs.toml --comment=\"first\"",
                  "lightnfs-ctl cluster catalog <show|status|history|diff|import|rollback|apply> [args...]",
                  "Shared export catalog (design 11): the versioned export list every gateway of the "
                  "cluster applies. Run `lightnfs-ctl help cluster catalog <command>` for details.",
                  "shared export catalog: show / diff / import / rollback / apply");
    cmd->add_subcommand(make_cluster_leaf(
        "cluster catalog show", "show", "lightnfs-ctl cluster catalog show", "lightnfs-ctl cluster catalog show",
        "Print the current catalog: a header line, then one line per export.", "print the current catalog"));
    cmd->add_subcommand(make_cluster_leaf("cluster catalog status", "status", "lightnfs-ctl cluster catalog status",
                                          "lightnfs-ctl cluster catalog status",
                                          "Every gateway's applied version, heartbeat and last apply result, "
                                          "then the latest version.",
                                          "applied version and last apply result per gateway"));
    cmd->add_subcommand(make_cluster_leaf("cluster catalog history", "history", "lightnfs-ctl cluster catalog history",
                                          "lightnfs-ctl cluster catalog history", "The catalog versions kept.",
                                          "list the versions kept"));
    cmd->add_subcommand(
        make_cluster_leaf("cluster catalog diff", "diff", "lightnfs-ctl cluster catalog diff 3 4",
                          "lightnfs-ctl cluster catalog diff [<v1>] [<v2>]",
                          "Export-level changes between two versions (default: this gateway's applied version vs the "
                          "current catalog).",
                          "export-level changes between two versions"));
    cmd->add_subcommand(make_cluster_leaf(
        "cluster catalog import", "import", "lightnfs-ctl cluster catalog import /etc/lightnfs/lightnfs.toml --dry-run",
        "lightnfs-ctl cluster catalog import <file> [--dry-run] [--comment=TEXT]",
        "Replace the catalog with the exports of a gateway-side TOML file (a local configuration or a "
        "catalog document; per-node keys are stripped). --dry-run only reports the diff.",
        "replace the catalog with the exports of a TOML file", kDryRun | kComment));
    cmd->add_subcommand(
        make_cluster_leaf("cluster catalog rollback", "rollback", "lightnfs-ctl cluster catalog rollback 3",
                          "lightnfs-ctl cluster catalog rollback <version> [--comment=TEXT]",
                          "Commit a kept version as a new one.", "commit a kept version as a new one", kComment));
    cmd->add_subcommand(make_cluster_leaf(
        "cluster catalog apply", "apply", "lightnfs-ctl cluster catalog apply", "lightnfs-ctl cluster catalog apply",
        "Apply the latest version on this gateway now (catalog_refresh = manual).", "apply the latest version now"));
    return cmd;
}

// `cluster export …` flags are parsed by the server (run_cluster_export_raw); they are
// registered here so `help cluster export <command>` lists them like any other option.
void add_export_flags(const Cmd& cmd, bool add) {
    if (add) {
        cmd->var<std::string>("path", "", "backend path of the export (required)");
        cmd->var<std::string>("fsid", "", "fsid of the export (required)");
        cmd->var<std::string>("backend", "", "backend kind (default: the server's local backend)");
        cmd->var<std::string>("opt", "", "backend cluster key, k=v (repeatable)");
    }
    cmd->var<std::string>("nodes", "", "gateways allowed to serve the export, comma-separated");
    cmd->var<std::string>("clients", "", "client allowlist, comma-separated");
    cmd->var<bool>("readonly", false, add ? "export read-only" : "export read-only (--readonly=false clears)");
    cmd->var<std::string>("squash", "", "id squashing: root, all or none");
    cmd->var<int>("anon-uid", 0, "uid squashed ids map to");
    cmd->var<int>("anon-gid", 0, "gid squashed ids map to");
    cmd->var<int>("read-bps", 0, "read bandwidth limit, bytes/s (0: none)");
    cmd->var<int>("write-bps", 0, "write bandwidth limit, bytes/s (0: none)");
    cmd->var<int>("iops", 0, "request rate limit, ops/s (0: none)");
    cmd->var<bool>("disabled", false, add ? "add the export disabled" : "disable the export (--disabled=false clears)");
    cmd->var<bool>("dry-run", false, "report the changes without committing");
    cmd->var<std::string>("comment", "", "audit-trail comment for the new version");
}

Cmd make_cluster_export() {
    auto cmd = make_node("export", "lightnfs-ctl cluster export add --path /srv/a --fsid 7 --nodes gw1,gw2",
                         "lightnfs-ctl cluster export <list|add|set|remove> [args...]",
                         "Single-export edits of the shared catalog (plan 12 D2); each edit commits a new "
                         "catalog version. Run `lightnfs-ctl help cluster export <command>` for details.",
                         "single-export catalog edits: list / add / set / remove");
    cmd->add_subcommand(make_cluster_leaf("cluster export list", "list", "lightnfs-ctl cluster export list",
                                          "lightnfs-ctl cluster export list", "Print the catalog's exports.",
                                          "print the catalog's exports"));
    auto add = make_cluster_leaf("cluster export add", "add",
                                 "lightnfs-ctl cluster export add --path /srv/a --fsid 7 --nodes gw1,gw2 --readonly",
                                 "lightnfs-ctl cluster export add --path=P --fsid=N [options]",
                                 "Add one export to the catalog. Reusing an fsid a kept version used for a different "
                                 "export is refused unless --force.",
                                 "add one export");
    add_export_flags(add, true);
    add->var<bool>("force", false, "reuse an fsid a kept version used differently");
    cmd->add_subcommand(add);
    auto set = make_cluster_leaf("cluster export set", "set",
                                 "lightnfs-ctl cluster export set 7 --clients 10.0.0.0/8 --readonly=false",
                                 "lightnfs-ctl cluster export set <fsid> [options]",
                                 "Change the on-line fields of one export; only the options given change. "
                                 "--path, --backend and --opt are fixed once added.",
                                 "change the on-line fields of one export");
    add_export_flags(set, false);
    cmd->add_subcommand(set);
    auto remove =
        make_cluster_leaf("cluster export remove", "remove", "lightnfs-ctl cluster export remove 7",
                          "lightnfs-ctl cluster export remove <fsid> [--force] [--dry-run] [--comment=TEXT]",
                          "Remove one export; refused while a gateway serves it, unless --force.", "remove one export");
    remove->var<bool>("force", false, "remove even while a gateway serves the export");
    remove->var<bool>("dry-run", false, "report the changes without committing");
    remove->var<std::string>("comment", "", "audit-trail comment for the new version");
    cmd->add_subcommand(remove);
    return cmd;
}

Cmd make_cluster() {
    auto cmd = make_node("cluster", "lightnfs-ctl cluster takeover 7",
                         "lightnfs-ctl cluster <command> [args...] [--socket=PATH] [--json]",
                         "Multi-gateway failover (design 09), active-active per-export ownership (design 10) and "
                         "the shared export catalog (design 11). Every command answers `cluster: not enabled` on "
                         "a single gateway. Run `lightnfs-ctl help cluster <command>` for details.",
                         "failover, per-export ownership and the shared export catalog");
    cmd->add_subcommand(make_cluster_leaf(
        "cluster status", "status", "lightnfs-ctl cluster status --json", "lightnfs-ctl cluster status",
        "This gateway's role, node, epoch, fence owner/age, shared_dir and the peer list; active-active "
        "adds one line per export (role, owner, address, fs epoch, fence age, grace, takeovers).",
        "role, epoch, fence owner and peers"));
    cmd->add_subcommand(make_cluster_leaf(
        "cluster exports", "exports", "lightnfs-ctl cluster exports gw2", "lightnfs-ctl cluster exports [<node>]",
        "The exports one gateway (default: this one) serves right now — fsid, path, role, fs epoch — the "
        "export → owner map v3 clients mount by.",
        "exports served by one gateway"));
    cmd->add_subcommand(make_cluster_leaf(
        "cluster takeover", "takeover", "lightnfs-ctl cluster takeover 7 --force",
        "lightnfs-ctl cluster takeover [<fsid>] [--force]",
        "Ask a standby gateway to take the fence and start serving; with <fsid> only that export moves. "
        "--force takes a live fence held by another node — only when that node is known to be down.",
        "take the fence and start serving", kForce));
    cmd->add_subcommand(make_cluster_leaf(
        "cluster standby", "standby", "lightnfs-ctl cluster standby", "lightnfs-ctl cluster standby [<fsid>]",
        "Drain an active gateway and release the fence; with <fsid> only that export is released.",
        "drain and release the fence"));
    cmd->add_subcommand(make_cluster_leaf("cluster migrate", "migrate", "lightnfs-ctl cluster migrate 7 gw2",
                                          "lightnfs-ctl cluster migrate <fsid> <node>",
                                          "On the owner: hand one export to a live peer without a client outage.",
                                          "hand one export to a live peer"));
    cmd->add_subcommand(make_cluster_catalog());
    cmd->add_subcommand(make_cluster_export());
    return cmd;
}

// Bench leaves rebuild the positional argv the bench entries have always parsed
// (argv[0] = the bench name); each entry _exit()s when its run completes.
void run_bench(const Cmd& c, int (*entry)(int, char**)) {
    std::vector<std::string> args;
    args.push_back(c->name());
    for (const auto& a : c->args()) args.push_back(a);
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) argv.push_back(s.data());
    g_exit = entry(static_cast<int>(argv.size()), argv.data());
}

Cmd make_bench_leaf(const char* name, const char* example, const char* usage, const char* help_short,
                    void (*run)(const Cmd&)) {
    return std::make_shared<ccmd::command>(name, example, usage,
                                           "Local benchmark: spins up an in-process stack and load-drives it over "
                                           "loopback; does not contact a running server. Terminates via _exit().",
                                           help_short, run);
}

Cmd make_bench() {
    auto cmd = make_node("bench", "lightnfs-ctl bench nullrpc 1 4 20000 32",
                         "lightnfs-ctl bench <echo|nullrpc|fullpath> [args...]",
                         "Three-layer benchmarks (design 02 §2.8): echo (L1 transport), nullrpc (L2 RPC, phase-0 "
                         "gate: exit 2 when a single reactor lands under 100k rps), fullpath (L4+ through the v3 "
                         "engine into the zero-latency memory backend).",
                         "three-layer benchmarks");
    cmd->add_subcommand(
        make_bench_leaf("echo", "lightnfs-ctl bench echo 1 4 20000 32 128",
                        "lightnfs-ctl bench echo [reactors=1] [conns=8] [per_conn=20000] [pipeline=32] [payload=128]",
                        "L1 transport echo", [](const Cmd& c) { run_bench(c, lnfs::bench::echo_main); }));
    cmd->add_subcommand(
        make_bench_leaf("nullrpc", "lightnfs-ctl bench nullrpc 1 4 20000 32",
                        "lightnfs-ctl bench nullrpc [reactors=1] [conns=8] [per_conn=50000] [pipeline=64]",
                        "L2 null-RPC (100k rps gate)", [](const Cmd& c) { run_bench(c, lnfs::bench::nullrpc_main); }));
    cmd->add_subcommand(make_bench_leaf(
        "fullpath", "lightnfs-ctl bench fullpath 1 4 20000 32 read",
        "lightnfs-ctl bench fullpath [reactors=1] [conns=8] [per_conn=50000] [pipeline=64] [proc=getattr|read]",
        "L4+ full path (memory backend)", [](const Cmd& c) { run_bench(c, lnfs::bench::fullpath_main); }));
    return cmd;
}

// Moves --socket/--json (folding `--socket PATH`/`-s PATH` into --socket=PATH) behind
// every positional so the leaf that owns the flag sees them wherever they were given
// (`lightnfs-ctl -s PATH cluster status` as well as `… cluster status -s PATH`).
std::vector<std::string> normalize_argv(int argc, char** argv) {
    std::vector<std::string> out, deferred;
    out.reserve(static_cast<size_t>(argc));
    if (argc > 0) out.emplace_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--socket" || a == "-s") && i + 1 < argc) a = "--socket=" + std::string(argv[++i]);
        if (a.rfind("--socket=", 0) == 0 || a == "--json" || a == "-j")
            deferred.push_back(std::move(a));
        else
            out.push_back(std::move(a));
    }
    out.insert(out.end(), deferred.begin(), deferred.end());
    return out;
}

// `cluster export …` (plan 12 D2) has too many per-export flags — and "was --readonly
// given at all" matters for `set` — to route through cflag: the line goes to the server
// verbatim (each argument quoted as needed), which parses and validates the flags and
// answers the usage line for a bad one.  --socket/-s and --json are still ours.
int run_cluster_export_raw(const std::vector<std::string>& argv) {
    std::string socket = default_socket(), line = "cluster export";
    bool json = false;
    for (size_t i = 3; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a.rfind("--socket=", 0) == 0)
            socket = a.substr(9);
        else if (a == "--json" || a == "-j")
            json = true;
        else
            line += " " + wire_arg(a);
    }
    if (json) line += " --json";
    return send_ctl(socket, line + "\n");
}

}  // namespace

int main(int argc, char** argv) {
    auto args = normalize_argv(argc, argv);
    if (args.size() >= 4 && args[1] == "cluster" && args[2] == "export") {
        // Help requests go through ccmd (which knows the flags); everything else is raw.
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) help |= args[i] == "--help" || args[i] == "-h";
        if (!help) return run_cluster_export_raw(args);
    }
    auto root = make_node("lightnfs-ctl", "lightnfs-ctl --socket=/var/lib/lightnfs/ctl.sock state",
                          "lightnfs-ctl <command> [args...] [--socket=PATH] [--json]",
                          "Admin CLI for a running lightnfsd (over the unix ctl socket) and host of the local "
                          "three-layer benchmarks. Run `lightnfs-ctl help <command>` for details.",
                          "lightnfs admin CLI");
    root->add_subcommand(make_socket_leaf("ping", "lightnfs-ctl ping", "lightnfs-ctl ping",
                                          "Liveness probe of the ctl socket.", "liveness probe"));
    root->add_subcommand(make_socket_leaf("metrics", "lightnfs-ctl metrics", "lightnfs-ctl metrics",
                                          "Full metrics dump in Prometheus text format (same content as the HTTP "
                                          "endpoint).",
                                          "Prometheus text metrics"));
    root->add_subcommand(
        make_socket_leaf("dump-errors", "lightnfs-ctl dump-errors", "lightnfs-ctl dump-errors",
                         "Most recent non-OK replies from the sampling ring (ts/peer/proc/xid/status) — "
                         "production triage without debug logging.",
                         "recent error replies"));
    root->add_subcommand(make_socket_leaf("drc", "lightnfs-ctl drc", "lightnfs-ctl drc [flush]",
                                          "Duplicate request cache statistics; `drc flush` drops every cached entry.",
                                          "DRC statistics / flush"));
    root->add_subcommand(make_socket_leaf("fdcache", "lightnfs-ctl fdcache", "lightnfs-ctl fdcache [flush]",
                                          "Per-export fd cache statistics; `fdcache flush` drops every unpinned entry.",
                                          "fd cache statistics / flush"));
    root->add_subcommand(make_socket_leaf("clear-poison", "lightnfs-ctl clear-poison", "lightnfs-ctl clear-poison",
                                          "Clear sticky fsync-EIO marks on every local export so COMMIT can succeed "
                                          "again after the underlying media problem is fixed.",
                                          "clear sticky fsync-EIO marks"));
    root->add_subcommand(make_socket_leaf("state", "lightnfs-ctl state", "lightnfs-ctl state",
                                          "v4 state tables: client/session/open/lock counts plus the table dumps.",
                                          "v4 state tables"));
    root->add_subcommand(make_socket_leaf("expire-client", "lightnfs-ctl expire-client 0x1a2b",
                                          "lightnfs-ctl expire-client <clientid>",
                                          "Forcibly reclaim every piece of state a client holds (triage for hung or "
                                          "leaked clients).",
                                          "force client expiry"));
    root->add_subcommand(make_socket_leaf("version", "lightnfs-ctl version", "lightnfs-ctl version",
                                          "Server version string.", "server version"));
    root->add_subcommand(
        make_socket_leaf("status", "lightnfs-ctl status", "lightnfs-ctl status",
                         "One-line server summary: version, uptime, connections, drain state, exports, "
                         "grace.",
                         "server status summary"));
    root->add_subcommand(make_socket_leaf(
        "loglevel", "lightnfs-ctl loglevel debug", "lightnfs-ctl loglevel <debug|info|warn|error>",
        "Change the log level of the running server (also reloadable via `reload`).", "set log level"));
    root->add_subcommand(make_socket_leaf("reload", "lightnfs-ctl reload", "lightnfs-ctl reload",
                                          "Re-read the config file and apply the hot-reloadable subset: log level, "
                                          "slow-request threshold, error ring, per-export client allowlists and QoS, "
                                          "per-client QoS. Topology changes are reported as restart-required.",
                                          "hot-reload configuration"));
    root->add_subcommand(make_socket_leaf("conns", "lightnfs-ctl conns", "lightnfs-ctl conns",
                                          "List live connections (id, peer, age).", "list connections"));
    root->add_subcommand(make_socket_leaf("kill-conn", "lightnfs-ctl kill-conn 42", "lightnfs-ctl kill-conn <id>",
                                          "Shut down one connection by id (see `conns`); the client sees a TCP reset "
                                          "and reconnects.",
                                          "kill one connection"));
    root->add_subcommand(make_socket_leaf("drain", "lightnfs-ctl drain", "lightnfs-ctl drain",
                                          "Stop accepting new connections while continuing to serve existing ones — "
                                          "graceful removal from a load balancer. Irreversible until restart.",
                                          "stop accepting connections"));
    root->add_subcommand(make_socket_leaf("grace-end", "lightnfs-ctl grace-end", "lightnfs-ctl grace-end",
                                          "End the post-restart grace period immediately (clients that have not "
                                          "reclaimed yet lose their claim window).",
                                          "end grace early"));
    root->add_subcommand(make_cluster());
    root->add_subcommand(make_bench());

    try {
        root->execute(args);
        return g_exit;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lightnfs-ctl: %s\n", e.what());
        return 2;
    }
}
