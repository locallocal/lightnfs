// lightnfsd entry point: argv handling and the ccmd command definition.  The process
// lifecycle (startup order, hot reload, shutdown) is server/daemon.

#include <sys/stat.h>

#include <ccmd.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server/daemon.hpp"

namespace {

// ccmd callbacks return void; the exit code travels through this
// (0 success / 1 runtime failure / 2 usage error).
int g_exit = 0;

// cflag takes long-option values only as --name=value; fold the `--config FILE`
// form used by the acceptance scripts and the systemd unit into that shape.
std::vector<std::string> normalize_argv(int argc, char** argv) {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      out.push_back("--config=" + std::string(argv[++i]));
    } else {
      out.push_back(std::move(a));
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  // Clients dictate creation modes over the wire; the server's own umask must not
  // subtract bits (the backend applies requested modes exactly).
  umask(0);

  auto root = std::make_shared<ccmd::c_command>(
      "lightnfsd", "lightnfsd --config=/etc/lightnfs/lightnfs.toml",
      "lightnfsd [--config=<path>] [--check-config]",
      "Userspace NFS gateway (NFSv3 + NFSv4.1/4.2). With no command the server runs "
      "until SIGINT/SIGTERM; --check-config validates the configuration and exits.",
      "userspace NFS gateway", [](const std::shared_ptr<ccmd::c_command>& c) {
        auto path = c->var<std::string>("config");
        g_exit = c->var<bool>("check-config") ? lnfs::server::check_config(path)
                                              : lnfs::server::run_server(path);
      });
  root->varp<std::string>("config", "c", "/etc/lightnfs/lightnfs.toml",
                          "Path to the lightnfs TOML config file");
  root->var<bool>("check-config", false, "Validate the configuration and exit");

  try {
    root->execute(normalize_argv(argc, argv));
    return g_exit;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lightnfsd: %s\n", e.what());
    return 2;
  }
}
