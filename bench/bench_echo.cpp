// Layer-1 benchmark (design 02 §2.8): raw record echo — accept, read_record, write the same
// chain back (zero-copy). Measures transport + record-marking overhead with no RPC parsing.
//
//   bench_echo [reactors=1] [conns=8] [per_conn=20000] [pipeline=32] [payload=128]

#include <cstdlib>

#include "bench/bench_util.hpp"
#include "runtime/io.hpp"
#include "runtime/runtime.hpp"
#include "transport/record_stream.hpp"
#include "util/log.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::transport;

namespace {

Task<void> echo_conn(int fd, BufferPool* pool) {
  RecordStream rs(fd, *pool, 1 << 20, (1 << 20) + (64 << 10));
  for (;;) {
    auto rec = co_await rs.read_record();
    if (!rec) break;
    auto wr = co_await rs.write_record(std::move(*rec));  // BufferChain == SendBuf: zero-copy
    if (!wr) break;
  }
  co_await uring_close(fd);
}

Task<void> echo_accept(int lfd, Runtime* rt, BufferPool* pool) {
  for (;;) {
    sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int cfd = co_await uring_accept(lfd, reinterpret_cast<sockaddr*>(&ss), &sl);
    if (cfd < 0) break;
    spawn(echo_conn(cfd, pool), rt->next());
  }
}

}  // namespace

int main(int argc, char** argv) {
  int reactors = argc > 1 ? atoi(argv[1]) : 1;
  int conns = argc > 2 ? atoi(argv[2]) : 8;
  uint64_t per_conn = argc > 3 ? strtoull(argv[3], nullptr, 10) : 20000;
  int pipeline = argc > 4 ? atoi(argv[4]) : 32;
  size_t payload = argc > 5 ? strtoull(argv[5], nullptr, 10) : 128;

  set_log_level(LogLevel::kWarn);
  Runtime rt(Runtime::Config{.reactors = reactors, .offload_threads = 2});
  static BufferPool pool;

  int lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1, zero = 0;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(lfd, 1024)) {
    perror("bind/listen");
    return 1;
  }
  socklen_t alen = sizeof(addr);
  getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
  uint16_t port = ntohs(addr.sin6_port);

  spawn(echo_accept(lfd, &rt, &pool), rt.reactor(0));
  rt.start();

  std::vector<std::byte> record(4 + payload);
  uint32_t hdr = xdr::to_be32(0x80000000u | (uint32_t)payload);
  memcpy(record.data(), &hdr, 4);
  memset(record.data() + 4, 'x', payload);

  auto res = bench::run_load(port, conns, per_conn, pipeline, record);
  printf("bench_echo: ring=%s reactors=%d conns=%d pipeline=%d payload=%zu -> "
         "%llu records in %.3fs = %.0f rps\n",
         rt.ring_kind().c_str(), reactors, conns, pipeline, payload,
         (unsigned long long)res.completed, res.seconds, res.rps());

  close(lfd);
  fflush(stdout);
  _exit(0);  // reactors may still be blocked in accept; process exit tears them down

}
