// lightnfs-ctl (design 08 §8.6): admin CLI over the server's unix ctl socket, plus
// the three-layer benchmarks (design 02 §2.8) as a local subcommand family that spins
// up its own in-process stack and never touches the socket.
//
// Command tree (ccmd, third_party/ccmd):
//   lightnfs-ctl <ping|metrics|dump-errors|drc|fdcache|clear-poison|state> [--socket=PATH]
//   lightnfs-ctl expire-client <clientid> [--socket=PATH]
//   lightnfs-ctl cluster <status|exports [<node>]|takeover [<fsid>] [--force]|standby
//   [<fsid>]|migrate <fsid> <node>|catalog <show|status|history|diff [v1] [v2]|
//   import <file> [--dry-run] [--comment=TEXT]|rollback <version>|apply>>
//                [--socket=PATH]
//   lightnfs-ctl cluster export <list|add …|set <fsid> …|remove <fsid> [--force]> [--socket=PATH]
//                (forwarded verbatim: the server parses the export flags, plan 12 D2)
//   lightnfs-ctl bench <echo|nullrpc|fullpath> [args...]
//
// Socket resolution: --socket, else $LIGHTNFS_CTL, else /tmp/lightnfs-state/ctl.sock.
// cflag takes long-option values only as --name=value; the historical spellings
// (`--socket PATH`, and placing it before the subcommand) are folded and reordered by
// normalize_argv so existing invocations keep working.

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

using Cmd = std::shared_ptr<ccmd::c_command>;

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

std::string default_socket() {
    const char* env = std::getenv("LIGHTNFS_CTL");
    return env ? env : "/tmp/lightnfs-state/ctl.sock";
}

// Root options do not propagate down in ccmd, so --socket/--json are registered per leaf.
void add_socket_flag(const Cmd& cmd) {
    cmd->varp<std::string>("socket", "s", default_socket(), "Path to the server ctl socket (default: $LIGHTNFS_CTL)");
    cmd->varp<bool>("json", "j", false, "JSON output (machine-readable)");
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
    auto cmd = std::make_shared<ccmd::c_command>(name, example, usage, help_long, help_short, run_socket_cmd);
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

// `cluster <status|takeover|standby|…>` (plan 10 C3): a socket leaf with a subcommand
// positional, like `drc [flush]`, plus the flags `takeover` (--force) and `catalog
// import` (--dry-run, --comment) forward.
void run_cluster_cmd(const Cmd& c) {
    std::string line = c->name();
    for (const auto& a : c->args()) line += " " + wire_arg(a);
    if (c->var<bool>("force")) line += " --force";
    if (c->var<bool>("dry-run")) line += " --dry-run";
    if (auto comment = c->var<std::string>("comment"); !comment.empty()) line += " --comment " + wire_arg(comment);
    if (c->var<bool>("json")) line += " --json";
    line += '\n';
    g_exit = send_ctl(c->var<std::string>("socket"), line);
}

Cmd make_cluster_leaf() {
    auto cmd = std::make_shared<ccmd::c_command>(
        "cluster", "lightnfs-ctl cluster catalog import /etc/lightnfs/lightnfs.toml --comment=\"first catalog\"",
        "lightnfs-ctl cluster <status|exports [<node>]|takeover [<fsid>] [--force]|standby "
        "[<fsid>]|migrate <fsid> <node>|catalog <show|status|history|diff [<v1>] [<v2>]|"
        "import <file> [--dry-run] [--comment=TEXT]|rollback <version>|apply>>",
        "Multi-gateway failover (design 09): `status` shows this gateway's role, node, "
        "epoch, fence owner/age, shared_dir and the peer list; `takeover` asks a standby "
        "gateway to take the fence and start serving (--force takes a live fence held by "
        "another node — only when that node is known to be down); `standby` drains an "
        "active gateway and releases the fence. Active-active (design 10): `status` "
        "prints one line per export (role, owner, address, fs epoch, fence age, grace, "
        "takeovers); `exports [<node>]` lists the exports one gateway (default: this one) "
        "serves right now — fsid, path, role, fs epoch — the export → owner map v3 clients "
        "mount by; `takeover <fsid>` and `standby <fsid>` move one export; `migrate "
        "<fsid> <node>` (on the owner) hands one export to a live peer without a client "
        "outage. Shared export catalog (design 11): `catalog show` prints the current "
        "catalog (header, one line per export); `catalog status` every gateway's applied "
        "version, heartbeat and last apply result, then the latest version; `catalog "
        "history` the versions kept; `catalog diff [<v1>] [<v2>]` the export-level "
        "changes between two versions (default: this gateway's applied version vs the "
        "current catalog); `catalog import <file>` replaces the catalog with the exports "
        "of a gateway-side TOML file (a local configuration or a catalog document; "
        "per-node keys stripped; --dry-run only reports the diff; --comment=TEXT goes into "
        "the audit trail); `catalog rollback <version>` commits a kept version as a new "
        "one; `catalog apply` applies the latest version now (catalog_refresh = manual). "
        "`export list` prints the catalog's exports; `export add --path P --fsid N "
        "[--backend B] [--nodes a,b] [--clients c1,c2] [--readonly] [--squash root|all|none] "
        "[--anon-uid N] [--anon-gid N] [--read-bps N] [--write-bps N] [--iops N] [--opt k=v …] "
        "[--disabled] [--force] [--dry-run] [--comment T]` adds one export (--opt sets a "
        "backend cluster key; --force reuses an fsid a kept version used differently); "
        "`export set <fsid> …` changes the on-line fields of one export (the same flags "
        "minus --path/--backend/--opt; --readonly=false / --disabled=false to clear); "
        "`export remove <fsid> [--force]` removes one (refused while a gateway serves it, "
        "unless --force). Answers `cluster: not enabled` on a single gateway.",
        "cluster role: status / exports [node] / takeover [fsid] / standby [fsid] / migrate "
        "fsid node / catalog show|status|history|diff|import|rollback|apply / export "
        "list|add|set|remove",
        run_cluster_cmd);
    add_socket_flag(cmd);
    cmd->varp<bool>("force", "f", false, "takeover: overwrite a live fence held by another node");
    cmd->varp<bool>("dry-run", "n", false, "catalog import: report the changes without committing");
    cmd->varp<std::string>("comment", "c", "", "catalog import / rollback: audit-trail comment for the new version");
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
    return std::make_shared<ccmd::c_command>(name, example, usage,
                                             "Local benchmark: spins up an in-process stack and load-drives it over "
                                             "loopback; does not contact a running server. Terminates via _exit().",
                                             help_short, run);
}

Cmd make_bench() {
    auto cmd = std::make_shared<ccmd::c_command>(
        "bench", "lightnfs-ctl bench nullrpc 1 4 20000 32", "lightnfs-ctl bench <echo|nullrpc|fullpath> [args...]",
        "Three-layer benchmarks (design 02 §2.8): echo (L1 transport), nullrpc (L2 RPC, "
        "phase-0 gate: exit 2 when a single reactor lands under 100k rps), fullpath "
        "(L4+ through the v3 engine into the zero-latency memory backend).",
        "three-layer benchmarks", [](const Cmd& c) {
            c->print_help();
            g_exit = 2;
        });
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

// Folds `--socket PATH`/`-s PATH` (and `--comment TEXT`) into --socket=PATH (the only
// form cflag takes) and
// moves a pre-subcommand --socket/--json to the end so the leaf that owns the flag
// sees it.
std::vector<std::string> normalize_argv(int argc, char** argv) {
    std::vector<std::string> out, deferred;
    out.reserve(static_cast<size_t>(argc));
    if (argc > 0) out.emplace_back(argv[0]);
    bool seen_cmd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--socket" || a == "-s") && i + 1 < argc) a = "--socket=" + std::string(argv[++i]);
        if ((a == "--comment" || a == "-c") && i + 1 < argc) a = "--comment=" + std::string(argv[++i]);
        if (!seen_cmd && (a.rfind("--socket=", 0) == 0 || a == "--json" || a == "-j")) {
            deferred.push_back(std::move(a));
            continue;
        }
        if (!a.empty() && a[0] != '-') seen_cmd = true;
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
    {
        auto args = normalize_argv(argc, argv);
        if (args.size() >= 3 && args[1] == "cluster" && args[2] == "export") return run_cluster_export_raw(args);
    }
    auto root = std::make_shared<ccmd::c_command>(
        "lightnfs-ctl", "lightnfs-ctl --socket=/var/lib/lightnfs/ctl.sock state",
        "lightnfs-ctl <command> [--socket=PATH] | lightnfs-ctl bench <name> [args...]",
        "Admin CLI for a running lightnfsd (over the unix ctl socket) and host of the "
        "local three-layer benchmarks. Run `lightnfs-ctl help <command>` for details.",
        "lightnfs admin CLI", [](const Cmd& c) {
            c->print_help();
            g_exit = 2;
        });
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
    root->add_subcommand(make_cluster_leaf());
    root->add_subcommand(make_bench());

    try {
        root->execute(normalize_argv(argc, argv));
        return g_exit;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lightnfs-ctl: %s\n", e.what());
        return 2;
    }
}
