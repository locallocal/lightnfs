// lightnfsd entry point: the ccmd command definition (both `--config=FILE` and
// `--config FILE` are accepted).  The process lifecycle (startup order, hot reload,
// shutdown) is server/daemon.

#include <sys/stat.h>

#include <ccmd.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "server/daemon.hpp"

namespace {

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

}  // namespace

int main(int argc, char** argv) {
    // Clients dictate creation modes over the wire; the server's own umask must not
    // subtract bits (the backend applies requested modes exactly).
    umask(0);

    auto root = std::make_shared<ccmd::command>(
        "lightnfsd", "lightnfsd --config=/etc/lightnfs/lightnfs.toml", "lightnfsd [--config=<path>] [--check-config]",
        "Userspace NFS gateway (NFSv3 + NFSv4.1/4.2). With no command the server runs "
        "until SIGINT/SIGTERM; --check-config validates the configuration and exits.",
        "userspace NFS gateway", [](const std::shared_ptr<ccmd::command>& c) {
            auto path = c->var<std::string>("config");
            g_exit = c->var<bool>("check-config") ? lnfs::server::check_config(path) : lnfs::server::run_server(path);
        });
    root->varp<std::string>("config", "c", "/etc/lightnfs/lightnfs.toml", "path to the lightnfs TOML config file");
    root->var<bool>("check-config", false, "validate the configuration and exit");

    try {
        root->execute(argc, argv);
        return g_exit;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lightnfsd: %s\n", e.what());
        return 2;
    }
}
