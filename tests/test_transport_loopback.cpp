// End-to-end over real sockets: Runtime + Listener + Dispatcher serving a test program;
// plain blocking-socket clients (like a real RPC client would look on the wire) exercise
// NULL and echo procedures, pipelining, and connection teardown.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

#include "mini_test.hpp"
#include "transport/listener.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;

namespace {

constexpr uint32_t kProg = 200001;

void add_test_program(Dispatcher& disp) {
  disp.add({kProg, 1, 1,
            [](ConnCtx& c, RpcCall& call, const Cred&) -> Task<void> {
              xdr::XdrEnc enc(c.pool);
              encode_reply_success(enc, call.xid);
              if (call.proc == 1) {  // echo
                auto arg = call.args.opaque(1 << 16);
                if (!arg) {
                  co_await Dispatcher::reply_garbage_args(c, call.xid);
                  co_return;
                }
                enc.opaque(*arg);
              }
              co_await c.send(enc.take());
            }});
}

int connect_loopback(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

std::vector<std::byte> build_call(uint32_t xid, uint32_t proc, std::string_view payload) {
  BufferPool pool;
  xdr::XdrEnc enc(pool);
  enc.u32(xid);
  enc.u32(0);  // CALL
  enc.u32(2);
  enc.u32(kProg);
  enc.u32(1);
  enc.u32(proc);
  enc.u32(0);
  enc.u32(0);  // cred AUTH_NONE
  enc.u32(0);
  enc.u32(0);  // verf
  if (proc == 1)
    enc.opaque(std::span<const std::byte>((const std::byte*)payload.data(), payload.size()));
  auto body = enc.take().to_bytes();
  std::vector<std::byte> rec(4 + body.size());
  uint32_t hdr = xdr::to_be32(0x80000000u | (uint32_t)body.size());
  std::memcpy(rec.data(), &hdr, 4);
  std::memcpy(rec.data() + 4, body.data(), body.size());
  return rec;
}

bool read_exact(int fd, std::byte* p, size_t n) {
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

// Reads one record; returns body bytes.
bool read_record(int fd, std::vector<std::byte>& out) {
  std::byte hdr[4];
  if (!read_exact(fd, hdr, 4)) return false;
  uint32_t h;
  std::memcpy(&h, hdr, 4);
  h = xdr::from_be32(h);
  size_t len = h & 0x7fffffffu;
  out.resize(len);
  return read_exact(fd, out.data(), len);
}

#define ASSERT_OK(r) ASSERT_TRUE(r.has_value())

struct Server {
  Runtime rt{Runtime::Config{.reactors = 2, .offload_threads = 2}};
  Dispatcher disp;
  std::unique_ptr<Listener> listener;

  Server() {
    add_test_program(disp);
    auto l = Listener::create(0, TransportConfig{}, disp, rt);
    ASSERT_OK(l);
    listener = std::move(*l);
    spawn(listener->run(), rt.reactor(0));
    rt.start();
  }
  ~Server() {
    listener->request_stop();
    rt.stop_and_join();
  }
  uint16_t port() { return listener->port(); }
};

}  // namespace

TEST(Loopback, NullAndEcho) {
  Server srv;
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);

  auto call0 = build_call(1, 0, "");
  ASSERT_TRUE(write(fd, call0.data(), call0.size()) == (ssize_t)call0.size());
  std::vector<std::byte> rep;
  ASSERT_TRUE(read_record(fd, rep));
  xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec.u32(), 1u);   // xid
  EXPECT_EQ(*dec.u32(), 1u);   // REPLY
  EXPECT_EQ(*dec.u32(), 0u);   // MSG_ACCEPTED

  auto call1 = build_call(2, 1, "the quick brown fox");
  ASSERT_TRUE(write(fd, call1.data(), call1.size()) == (ssize_t)call1.size());
  ASSERT_TRUE(read_record(fd, rep));
  xdr::XdrDec dec2{std::span<const std::byte>(rep.data(), rep.size())};
  EXPECT_EQ(*dec2.u32(), 2u);
  (void)dec2.u32();  // REPLY
  (void)dec2.u32();  // MSG_ACCEPTED
  (void)dec2.u32();  // verf flavor
  (void)dec2.u32();  // verf len
  EXPECT_EQ(*dec2.u32(), 0u);  // accept stat SUCCESS
  auto echoed = *dec2.opaque(1 << 16);
  EXPECT_STREQ(std::string((const char*)echoed.data(), echoed.size()), "the quick brown fox");

  close(fd);
}

TEST(Loopback, PipelinedRequests) {
  Server srv;
  int fd = connect_loopback(srv.port());
  ASSERT_TRUE(fd >= 0);
  // fire 50 calls back-to-back before reading any reply
  std::vector<std::byte> all;
  for (uint32_t i = 0; i < 50; ++i) {
    auto c = build_call(100 + i, 1, "payload-" + std::to_string(i));
    all.insert(all.end(), c.begin(), c.end());
  }
  ASSERT_TRUE(write(fd, all.data(), all.size()) == (ssize_t)all.size());
  // replies may arrive in any order; collect xids
  std::vector<uint32_t> xids;
  for (int i = 0; i < 50; ++i) {
    std::vector<std::byte> rep;
    ASSERT_TRUE(read_record(fd, rep));
    xdr::XdrDec dec{std::span<const std::byte>(rep.data(), rep.size())};
    xids.push_back(*dec.u32());
  }
  std::sort(xids.begin(), xids.end());
  for (uint32_t i = 0; i < 50; ++i) EXPECT_EQ(xids[i], 100 + i);
  close(fd);
}

TEST(Loopback, ManyConnections) {
  Server srv;
  std::vector<std::thread> clients;
  std::atomic<int> ok{0};
  for (int t = 0; t < 8; ++t) {
    clients.emplace_back([&srv, &ok, t] {
      int fd = connect_loopback(srv.port());
      if (fd < 0) return;
      for (int i = 0; i < 20; ++i) {
        auto c = build_call(t * 1000 + i, 1, "x");
        if (write(fd, c.data(), c.size()) != (ssize_t)c.size()) return;
        std::vector<std::byte> rep;
        if (!read_record(fd, rep)) return;
      }
      close(fd);
      ok.fetch_add(1);
    });
  }
  for (auto& c : clients) c.join();
  EXPECT_EQ(ok.load(), 8);
}
