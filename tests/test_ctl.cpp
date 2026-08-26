// Management-plane hardening (plan doc 10 §1.8): ctl socket permissions + fragmented
// command framing, metrics bind address + CIDR allowlist, and the new config keys.

#include "mini_test.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "core/config.hpp"
#include "runtime/runtime.hpp"
#include "server/ctl.hpp"

using namespace lnfs;

TEST(Ctl, FragmentedCommandFramingAndSocketPerms) {
  char tmpl[] = "/tmp/lnfs-ctl-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string sock = dir + "/ctl.sock";
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();
  auto ctl = server::CtlServer::create(sock, {});
  ASSERT_TRUE(ctl.has_value());

  // Owner-only permissions regardless of umask.
  struct stat st {};
  ASSERT_TRUE(::stat(sock.c_str(), &st) == 0);
  EXPECT_EQ(static_cast<unsigned>(st.st_mode & 0777), 0600u);

  rt::spawn((*ctl)->run(), runtime.reactor(0));

  // A command split across two sends used to be truncated at the first recv.
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_TRUE(fd >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  sock.copy(addr.sun_path, sock.size());
  ASSERT_TRUE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  ASSERT_TRUE(::write(fd, "pi", 2) == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(::write(fd, "ng\n", 3) == 3);
  std::string got;
  char buf[64];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) got.append(buf, static_cast<size_t>(n));
  ::close(fd);
  EXPECT_STREQ(got, "pong\n");

  (*ctl)->request_stop();
  runtime.stop_and_join();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

namespace {

int connect_metrics(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string http_get(int fd) {
  const char req[] = "GET /metrics HTTP/1.0\r\n\r\n";
  (void)!::write(fd, req, sizeof req - 1);
  std::string got;
  char buf[512];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) got.append(buf, static_cast<size_t>(n));
  return got;
}

}  // namespace

TEST(Ctl, MetricsBindLoopbackAndCidrAcl) {
  rt::Runtime runtime({.reactors = 1, .offload_threads = 1});
  runtime.start();

  // Empty allowlist: loopback bind serves the response.
  auto open_ep = server::MetricsHttp::create(0, "127.0.0.1", {});
  ASSERT_TRUE(open_ep.has_value());
  rt::spawn((*open_ep)->run(), runtime.reactor(0));
  int fd = connect_metrics((*open_ep)->port());
  ASSERT_TRUE(fd >= 0);
  auto body = http_get(fd);
  ::close(fd);
  EXPECT_TRUE(body.starts_with("HTTP/1.0 200 OK"));

  // Allowlist that excludes loopback: connection is dropped without a response.
  auto deny_cidr = core::Cidr::parse("10.0.0.0/8");
  ASSERT_TRUE(deny_cidr.has_value());
  std::vector<core::Cidr> allow{*deny_cidr};
  auto denied_ep = server::MetricsHttp::create(0, "127.0.0.1", std::move(allow));
  ASSERT_TRUE(denied_ep.has_value());
  rt::spawn((*denied_ep)->run(), runtime.reactor(0));
  fd = connect_metrics((*denied_ep)->port());
  ASSERT_TRUE(fd >= 0);
  EXPECT_STREQ(http_get(fd), "");
  ::close(fd);

  (*open_ep)->request_stop();
  (*denied_ep)->request_stop();
  runtime.stop_and_join();
}

TEST(Ctl, MetricsAndIdentityConfigKeys) {
  auto parsed = core::parse_config(
      "[server]\n"
      "metrics_bind = \"::1\"\n"
      "metrics_allow = [\"127.0.0.0/8\", \"::1/128\"]\n"
      "server_owner = \"nodeA\"\n"
      "server_scope = \"clusterX\"\n");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_STREQ(parsed->server.metrics_bind, "::1");
  EXPECT_EQ(parsed->server.metrics_allow.size(), 2u);
  EXPECT_STREQ(parsed->server.server_owner, "nodeA");
  EXPECT_STREQ(parsed->server.server_scope, "clusterX");
}
