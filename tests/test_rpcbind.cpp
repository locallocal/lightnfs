// server/rpcbind.cpp coverage (plan doc 10 §7.1): a fake portmapper on an ephemeral UDP
// port (rpcbind_target_port test seam) answers one call per case, exercising request
// encoding and every reply-parse branch: success, padded verifier, short reply, xid
// mismatch, MSG_DENIED, accept_stat failure, and a false bool result.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>
#include <vector>

#include "mini_test.hpp"
#include "server/rpcbind.hpp"
#include "util/errno.hpp"

using namespace lnfs;

namespace {

uint32_t get32(const std::byte* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return ntohl(v);
}

void put32(std::vector<std::byte>& out, uint32_t v) {
  v = htonl(v);
  const std::byte* p = reinterpret_cast<const std::byte*>(&v);
  out.insert(out.end(), p, p + 4);
}

enum class Reply {
  kSuccess,        // well-formed accept, bool true
  kPaddedVerf,     // same, but with an 8-byte AUTH verifier before the accept body
  kShort,          // 8 bytes only
  kBadXid,         // xid+1
  kDenied,         // reply_stat = MSG_DENIED
  kAcceptFailure,  // accept_stat = PROG_UNAVAIL
  kBoolFalse,      // accepted, but the portmapper answered false
};

struct FakePortmapper {
  int fd = -1;
  uint16_t port = 0;
  std::thread th;
  // What the last request carried (checked by the encoding test).
  uint32_t req_prog = 0, req_vers = 0, req_proc = 0, req_arg_prog = 0, req_arg_port = 0;

  explicit FakePortmapper(Reply mode) {
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ASSERT_TRUE(fd >= 0);
    timeval to{5, 0};  // a lost datagram must not hang the suite
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_TRUE(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    socklen_t len = sizeof(a);
    ASSERT_TRUE(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    port = ntohs(a.sin_port);
    th = std::thread([this, mode] { serve_one(mode); });
  }
  ~FakePortmapper() {
    if (th.joinable()) th.join();
    if (fd >= 0) close(fd);
  }

  void serve_one(Reply mode) {
    std::byte req[256];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(fd, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 56) return;  // caller will time out and the test fails on the result
    uint32_t xid = get32(req);
    req_prog = get32(req + 12);
    req_vers = get32(req + 16);
    req_proc = get32(req + 20);
    req_arg_prog = get32(req + 40);
    req_arg_port = get32(req + 52);
    std::vector<std::byte> rep;
    put32(rep, mode == Reply::kBadXid ? xid + 1 : xid);
    put32(rep, 1);                                 // REPLY
    put32(rep, mode == Reply::kDenied ? 1 : 0);    // MSG_ACCEPTED / MSG_DENIED
    put32(rep, 0);                                 // verifier flavor
    if (mode == Reply::kPaddedVerf) {
      put32(rep, 8);  // verifier length: parser must skip 8 payload bytes
      put32(rep, 0xdeadbeef);
      put32(rep, 0xfeedface);
    } else {
      put32(rep, 0);
    }
    put32(rep, mode == Reply::kAcceptFailure ? 1 : 0);  // accept_stat
    put32(rep, mode == Reply::kBoolFalse ? 0 : 1);      // bool result
    size_t send_len = mode == Reply::kShort ? 8 : rep.size();
    sendto(fd, rep.data(), send_len, 0, reinterpret_cast<sockaddr*>(&from), fl);
  }
};

}  // namespace

TEST(Rpcbind, SetEncodesCallAndParsesSuccess) {
  FakePortmapper pm(Reply::kSuccess);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  EXPECT_TRUE(r.has_value());
  pm.th.join();
  EXPECT_EQ(pm.req_prog, 100000u);  // portmapper program
  EXPECT_EQ(pm.req_vers, 2u);      // PMAP v2
  EXPECT_EQ(pm.req_proc, 1u);      // PMAPPROC_SET
  EXPECT_EQ(pm.req_arg_prog, 100003u);
  EXPECT_EQ(pm.req_arg_port, 2049u);
}

TEST(Rpcbind, UnsetUsesProcTwo) {
  FakePortmapper pm(Reply::kSuccess);
  server::rpcbind_target_port(pm.port);
  EXPECT_TRUE(server::rpcbind_unset(100003, 3).has_value());
  pm.th.join();
  EXPECT_EQ(pm.req_proc, 2u);  // PMAPPROC_UNSET
  EXPECT_EQ(pm.req_arg_port, 0u);
}

TEST(Rpcbind, PaddedVerifierIsSkipped) {
  FakePortmapper pm(Reply::kPaddedVerf);
  server::rpcbind_target_port(pm.port);
  EXPECT_TRUE(server::rpcbind_set(100005, 3, 20048).has_value());
}

TEST(Rpcbind, ShortReplyIsEproto) {
  FakePortmapper pm(Reply::kShort);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  ASSERT_TRUE(!r.has_value());
  EXPECT_EQ(raw(r.error()), EPROTO);
}

TEST(Rpcbind, XidMismatchIsEproto) {
  FakePortmapper pm(Reply::kBadXid);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  ASSERT_TRUE(!r.has_value());
  EXPECT_EQ(raw(r.error()), EPROTO);
}

TEST(Rpcbind, MsgDeniedIsEproto) {
  FakePortmapper pm(Reply::kDenied);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  ASSERT_TRUE(!r.has_value());
  EXPECT_EQ(raw(r.error()), EPROTO);
}

TEST(Rpcbind, AcceptFailureIsEacces) {
  FakePortmapper pm(Reply::kAcceptFailure);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  ASSERT_TRUE(!r.has_value());
  EXPECT_EQ(raw(r.error()), EACCES);
}

TEST(Rpcbind, FalseResultIsEacces) {
  FakePortmapper pm(Reply::kBoolFalse);
  server::rpcbind_target_port(pm.port);
  auto r = server::rpcbind_set(100003, 3, 2049);
  ASSERT_TRUE(!r.has_value());
  EXPECT_EQ(raw(r.error()), EACCES);
}
