// Fuzz target feeding raw bytes into the full request path (design 03 §3.6, security list
// item 1): record bytes -> Dispatcher::handle_request (RPC header parse, auth parse, arg
// decode) over a FakeRing reactor. Any crash/UB is a finding; replies are drained and
// discarded.
//
// Built with -fsanitize=fuzzer,address when LNFS_BUILD_FUZZ=ON (clang); otherwise linked
// into fuzz_regress which replays corpus files (see regress_main.cpp).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <filesystem>
#include <vector>

#include "backend/memory.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "mountd/mount3.hpp"
#include "core/pseudofs.hpp"
#include "nfsv3/engine.hpp"
#include "nfsv4/engine.hpp"
#include "state/state_mgr.hpp"
#include "rpc/dispatch.hpp"
#include "runtime/reactor.hpp"
#include "runtime/testing/fake_ring.hpp"
#include "transport/connection.hpp"
#include "util/log.hpp"
#include "xdr/xdr.hpp"

using namespace lnfs;
using namespace lnfs::rt;
using namespace lnfs::rpc;
using namespace lnfs::transport;

namespace {

constexpr uint32_t kFuzzProg = 300000;

// Hermetic state_dir (plan doc 10 §7.2): a per-process private directory under $TMPDIR,
// removed at exit.  The old fixed "/tmp/lnfs-fuzz-state" made every EXCHANGE_ID exec
// write+fsync into a world-shared path and leak state across runs.
std::string g_state_dir;
void cleanup_state_dir() {
  std::error_code ec;
  if (!g_state_dir.empty()) std::filesystem::remove_all(g_state_dir, ec);
}
const std::string& fuzz_state_dir() {
  [[maybe_unused]] static bool init = [] {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/lnfs-fuzz-XXXXXX";
    std::vector<char> buf(tmpl.c_str(), tmpl.c_str() + tmpl.size() + 1);
    if (const char* got = mkdtemp(buf.data())) g_state_dir = got;
    std::atexit(cleanup_state_dir);
    return true;
  }();
  return g_state_dir;
}

// A handler that exercises the decoder shapes engines will use.
Task<void> fuzz_handler(void*, ConnCtx& c, RpcCall& call, const Cred&) {
  auto u = call.args.u32();
  auto o = call.args.opaque(64 << 10);
  auto s = call.args.string(4096);
  auto v = call.args.u64();
  (void)u;
  (void)s;
  (void)v;
  xdr::XdrEnc enc(c.pool);
  encode_reply_success(enc, call.xid);
  if (o) enc.opaque(*o);
  co_await c.send(enc.take());
}

}  // namespace

extern "C" void lnfs_fuzz_entry(const uint8_t* data, size_t size) {
  [[maybe_unused]] static bool quiet = [] {
    lnfs::set_log_level(lnfs::LogLevel::kError);
    return true;
  }();
  testing::FakeRing ring;
  Reactor r(ring);
  BufferPool pool;
  TransportConfig cfg;
  Peer peer;
  auto* addr = reinterpret_cast<sockaddr_in*>(&peer.addr);
  addr->sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr);
  peer.len = sizeof(*addr);
  ConnCtx ctx(3, peer, pool, cfg);
  Dispatcher disp;
  disp.add({kFuzzProg, 1, 1, nullptr, fuzz_handler});
  core::ExportTable exports;
  core::ExportConfig export_cfg;
  export_cfg.path = "/fuzz";
  export_cfg.fsid = 1;
  export_cfg.clients = {"127.0.0.0/8"};
  auto memory = std::make_unique<backend::MemoryBackend>(1);
  (void)exports.add(export_cfg, std::move(memory));
  std::array<std::byte, 16> key{};
  auto handles = core::FileHandleCodec::from_key(key, exports);
  core::ObjLockRegistry locks;
  nfsv3::Engine nfs(exports, handles, locks);
  mountd::Mount3 mount(exports, handles);
  nfs.register_with(disp);
  mount.register_with(disp);
  // v4.1 stack: COMPOUND decode surface + session/state machinery (phase 3).
  core::PseudoFs pseudo(exports);
  state::StateMgr state({.boot_epoch = 1, .state_dir = fuzz_state_dir()});
  nfsv4::Engine nfs4(exports, handles, locks, pseudo, state);
  nfs4.register_with(disp);

  BufferChain rec;
  if (size > 0) {
    // split input across two segments to exercise chain-spanning decode paths
    auto b = pool.alloc(size);
    std::memcpy(b.data(), data, size);
    uint32_t half = static_cast<uint32_t>(size / 2);
    rec.append(b, 0, half);
    rec.append(b, half, static_cast<uint32_t>(size - half));
  }

  spawn(
      [](Dispatcher* d, ConnCtx* c, BufferChain rr) -> Task<void> {
        co_await d->handle_request(*c, std::move(rr));
      }(&disp, &ctx, std::move(rec)),
      r);

  // Drain: complete any reply sends until the reactor is idle.
  for (int guard = 0; guard < 1000; ++guard) {
    bool progress = r.poll_once();
    while (ring.has_pending(testing::FakeRing::Kind::kSendv)) {
      auto op = ring.take(testing::FakeRing::Kind::kSendv);
      size_t total = 0;
      for (int i = 0; i < op.iovcnt; ++i) total += op.iov[i].iov_len;
      ring.complete(op, static_cast<int32_t>(total));
      progress = true;
    }
    if (!progress && r.live_tasks() == 0) break;
  }
}

#ifndef LNFS_FUZZ_REGRESS
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lnfs_fuzz_entry(data, size);
  return 0;
}
#endif
