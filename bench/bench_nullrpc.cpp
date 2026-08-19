// Layer-2 benchmark (design 02 §2.8): null-RPC through the full transport+RPC stack
// (record marking, header parse, auth, dispatch, reply encode). Phase-0 exit gate
// (roadmap stage 0): >= 100k rps on a single reactor.
//
//   bench_nullrpc [reactors=1] [conns=8] [per_conn=50000] [pipeline=64]

#include <cstdlib>

#include "bench/bench_util.hpp"
#include "transport/listener.hpp"
#include "util/log.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;

namespace {
constexpr uint32_t kProg = 100003;  // where the NFS program will live

std::vector<std::byte> build_null_call() {
  BufferPool pool;
  xdr::XdrEnc enc(pool);
  enc.u32(42);  // xid (constant: server does not care in this bench)
  enc.u32(0);   // CALL
  enc.u32(2);
  enc.u32(kProg);
  enc.u32(3);
  enc.u32(0);  // NULL proc
  enc.u32(1);  // AUTH_SYS cred, minimal body
  xdr::XdrEnc body(pool);
  body.u32(0);
  body.string("bench");
  body.u32(0);
  body.u32(0);
  body.u32(0);
  auto bb = body.take().to_bytes();
  enc.opaque(bb);
  enc.u32(0);  // verf
  enc.u32(0);
  auto payload = enc.take().to_bytes();
  std::vector<std::byte> rec(4 + payload.size());
  uint32_t hdr = xdr::to_be32(0x80000000u | (uint32_t)payload.size());
  memcpy(rec.data(), &hdr, 4);
  memcpy(rec.data() + 4, payload.data(), payload.size());
  return rec;
}
}  // namespace

int main(int argc, char** argv) {
  int reactors = argc > 1 ? atoi(argv[1]) : 1;
  int conns = argc > 2 ? atoi(argv[2]) : 8;
  uint64_t per_conn = argc > 3 ? strtoull(argv[3], nullptr, 10) : 50000;
  int pipeline = argc > 4 ? atoi(argv[4]) : 64;

  set_log_level(LogLevel::kWarn);
  Runtime rt(Runtime::Config{.reactors = reactors, .offload_threads = 2});
  Dispatcher disp;
  disp.add({kProg, 3, 3, [](ConnCtx& c, RpcCall& call, const Cred&) -> Task<void> {
              xdr::XdrEnc enc(c.pool);
              encode_reply_success(enc, call.xid);
              co_await c.send(enc.take());
            }});

  auto l = Listener::create(0, TransportConfig{}, disp, rt);
  if (!l) {
    fprintf(stderr, "listener failed\n");
    return 1;
  }
  spawn((*l)->run(), rt.reactor(0));
  rt.start();

  auto res = bench::run_load((*l)->port(), conns, per_conn, pipeline, build_null_call());
  double rps = res.rps();
  printf("bench_nullrpc: ring=%s reactors=%d conns=%d pipeline=%d -> "
         "%llu calls in %.3fs = %.0f rps%s\n",
         rt.ring_kind().c_str(), reactors, conns, pipeline,
         (unsigned long long)res.completed, res.seconds, rps,
         (reactors == 1 && rps < 100000) ? "  [BELOW 100k SINGLE-REACTOR TARGET]" : "");
  fflush(stdout);
  _exit(reactors == 1 && rps < 100000 ? 2 : 0);
}
