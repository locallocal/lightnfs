// lightnfs-ctl (design 08 §8.6, minimal): sends one command over the server's unix
// admin socket and prints the text reply.
//
//   lightnfs-ctl [--socket PATH] <ping|metrics|dump-errors|drc|fdcache|state|expire-client ID>
//
// Default socket: $LIGHTNFS_CTL, else /tmp/lightnfs-state/ctl.sock.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  const char* env = std::getenv("LIGHTNFS_CTL");
  std::string path = env ? env : "/tmp/lightnfs-state/ctl.sock";
  int argi = 1;
  if (argi + 1 < argc && std::string(argv[argi]) == "--socket") {
    path = argv[argi + 1];
    argi += 2;
  }
  if (argi >= argc) {
    std::fprintf(stderr,
                 "usage: lightnfs-ctl [--socket PATH] <ping|metrics|dump-errors|drc|fdcache|state|expire-client ID>\n");
    return 2;
  }
  std::string cmd = argv[argi];
  for (int i = argi + 1; i < argc; ++i) cmd += std::string(" ") + argv[i];
  cmd += '\n';

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
    return 1;
  }
  if (::write(fd, cmd.data(), cmd.size()) != static_cast<ssize_t>(cmd.size())) {
    std::fprintf(stderr, "write failed\n");
    return 1;
  }
  ::shutdown(fd, SHUT_WR);
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, static_cast<size_t>(n), stdout);
  ::close(fd);
  return 0;
}
