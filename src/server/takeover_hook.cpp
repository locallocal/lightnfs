#include "server/takeover_hook.hpp"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include "util/log.hpp"

extern char** environ;

namespace lnfs::server {

namespace {

// The daemon's environment minus any stale LNFS_* takeover variables, plus ours.
std::vector<std::string> hook_environment(const backend::ClusterIdentity& id,
                                          std::string_view prev_node) {
  static constexpr std::string_view kOurs[] = {"LNFS_CLUSTER_ID=", "LNFS_NODE=",
                                               "LNFS_EPOCH=", "LNFS_PREV_NODE="};
  std::vector<std::string> env;
  for (char** e = environ; e && *e; ++e) {
    std::string_view entry(*e);
    bool ours = false;
    for (auto k : kOurs) ours |= entry.starts_with(k);
    if (!ours) env.emplace_back(entry);
  }
  env.push_back("LNFS_CLUSTER_ID=" + id.cluster_id);
  env.push_back("LNFS_NODE=" + id.node);
  env.push_back("LNFS_EPOCH=" + std::to_string(id.epoch));
  env.push_back("LNFS_PREV_NODE=" + std::string(prev_node));
  return env;
}

}  // namespace

Result<void> run_takeover_hook(const std::string& path, const backend::ClusterIdentity& id,
                               std::string_view prev_node, std::chrono::milliseconds timeout) {
  auto env = hook_environment(id, prev_node);
  std::vector<char*> envp;
  envp.reserve(env.size() + 1);
  for (auto& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  std::string arg0 = path;
  char* argv[] = {arg0.data(), nullptr};

  // posix_spawn rather than fork(): the daemon is multi-threaded (reactors, io_uring
  // workers), and spawn keeps the child out of every post-fork trap.
  pid_t pid = 0;
  if (int rc = ::posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, envp.data()); rc != 0) {
    LNFS_WARN("cluster: takeover hook {} cannot start: {}", path, errno_name(errno_from(rc)));
    return Err(errno_from(rc));
  }
  LNFS_INFO("cluster: takeover hook {} started (pid {}, node={} epoch={} prev={})", path, pid,
            id.node, id.epoch, prev_node);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  for (;;) {
    pid_t got = ::waitpid(pid, &status, WNOHANG);
    if (got == pid) break;
    if (got < 0 && errno != EINTR) {
      LNFS_WARN("cluster: takeover hook {}: waitpid failed: {}", path,
                errno_name(errno_from(errno)));
      return Err(errno_from(errno));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      LNFS_WARN("cluster: takeover hook {} killed after {} ms", path, timeout.count());
      return Err(errno_from(ETIMEDOUT));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    LNFS_INFO("cluster: takeover hook {} done", path);
    return {};
  }
  if (WIFEXITED(status))
    LNFS_WARN("cluster: takeover hook {} exited with status {}", path, WEXITSTATUS(status));
  else if (WIFSIGNALED(status))
    LNFS_WARN("cluster: takeover hook {} killed by signal {}", path, WTERMSIG(status));
  return Err(errno_from(EIO));
}

}  // namespace lnfs::server
