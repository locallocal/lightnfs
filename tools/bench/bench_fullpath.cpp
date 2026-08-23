// Layer-4 benchmark (design 02 §2.8, third tier): full path through transport + RPC +
// v3 engine + core (handles, locks, errmap) into the zero-latency in-memory backend.
// Measures protocol-stack overhead beyond null-RPC: GETATTR (metadata path) and READ
// (data path with attach()).
//
//   lightnfs-ctl bench fullpath [reactors=1] [conns=8] [per_conn=50000] [pipeline=64] [proc=getattr|read]

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>

#include "backend/memory.hpp"
#include "tools/bench/bench_main.hpp"
#include "tools/bench/bench_util.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "nfsv3/engine.hpp"
#include "transport/listener.hpp"
#include "util/log.hpp"

using namespace lnfs;
using namespace lnfs::rt;

namespace {

std::vector<std::byte> build_record(BufferPool& pool, uint32_t proc,
                                    const std::vector<std::byte>& fh) {
  xdr::XdrEnc enc(pool);
  enc.u32(7);  // xid (idempotent procs: constant is fine)
  enc.u32(0);
  enc.u32(2);
  enc.u32(nfsv3::kProgram);
  enc.u32(nfsv3::kVersion);
  enc.u32(proc);
  enc.u32(0);
  enc.u32(0);
  enc.u32(0);
  enc.u32(0);
  enc.opaque(fh);
  if (proc == 6) {  // READ: offset 0, count 4096
    enc.u64(0);
    enc.u32(4096);
  }
  auto payload = enc.take().to_bytes();
  std::vector<std::byte> rec(4 + payload.size());
  uint32_t hdr = xdr::to_be32(0x80000000u | (uint32_t)payload.size());
  memcpy(rec.data(), &hdr, 4);
  memcpy(rec.data() + 4, payload.data(), payload.size());
  return rec;
}

}  // namespace

int lnfs::bench::fullpath_main(int argc, char** argv) {
  int reactors = argc > 1 ? atoi(argv[1]) : 1;
  int conns = argc > 2 ? atoi(argv[2]) : 8;
  uint64_t per_conn = argc > 3 ? strtoull(argv[3], nullptr, 10) : 50000;
  int pipeline = argc > 4 ? atoi(argv[4]) : 64;
  bool read_proc = argc > 5 && std::string_view(argv[5]) == "read";

  set_log_level(LogLevel::kWarn);
  Runtime rt(Runtime::Config{.reactors = reactors, .offload_threads = 2});

  core::ExportTable exports;
  auto mem = std::make_unique<backend::MemoryBackend>(9);
  auto* memory = mem.get();
  (void)memory->add_file("/bench.bin", std::string(4096, 'x'));
  core::ExportConfig cfg;
  cfg.path = "/bench";
  cfg.fsid = 9;
  cfg.clients = {"127.0.0.0/8", "::/0"};
  cfg.squash = core::Squash::kNone;
  (void)exports.add(cfg, std::move(mem));

  std::array<std::byte, 16> key{};
  auto handles = core::FileHandleCodec::from_key(key, exports);
  core::ObjLockRegistry locks;
  nfsv3::Engine engine(exports, handles, locks);
  rpc::Dispatcher disp;
  engine.register_with(disp);

  auto l = transport::Listener::create(0, transport::TransportConfig{}, disp, rt);
  if (!l) {
    fprintf(stderr, "listener failed\n");
    return 1;
  }
  spawn((*l)->run(), rt.reactor(0));
  rt.start();

  // Resolve the target fh directly (bench, not protocol conformance).
  std::vector<std::byte> fh;
  {
    BufferPool pool;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    spawn(
        [](backend::MemoryBackend* m, core::ExportTable* exports,
           core::FileHandleCodec* handles, std::vector<std::byte>* out, std::mutex* mu,
           std::condition_variable* cv, bool* done, bool read_proc) -> Task<void> {
          auto root = co_await m->root();
          backend::Cred cred{0, 0, {}};
          backend::ObjPtr target = *root;
          if (read_proc) target = *co_await (*root)->lookup(cred, "bench.bin");
          *out = handles->encode(*exports->by_fsid(9), target->id());
          {
            std::lock_guard lk(*mu);
            *done = true;
            cv->notify_one();
          }
        }(memory, &exports, &handles, &fh, &mu, &cv, &done, read_proc),
        rt.reactor(0));
    std::unique_lock lk(mu);
    cv.wait(lk, [&] { return done; });
  }

  BufferPool pool;
  auto record = build_record(pool, read_proc ? 6u : 1u, fh);
  auto res = bench::run_load((*l)->port(), conns, per_conn, pipeline, record);
  printf("bench_fullpath: proc=%s ring=%s reactors=%d conns=%d pipeline=%d -> "
         "%llu calls in %.3fs = %.0f rps\n",
         read_proc ? "READ4k" : "GETATTR", rt.ring_kind().c_str(), reactors, conns,
         pipeline, (unsigned long long)res.completed, res.seconds, res.rps());
  fflush(stdout);
  _exit(0);
}
