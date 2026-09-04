#pragma once
// The server's durable identity and protocol engines, assembled once per process
// (design 01 §1.4 startup order: config → identity → backends → engines → listeners).
//
//  - CoreState: what survives restarts and is shared by every engine — the export
//    table, the file-handle HMAC codec bound to it, the boot epoch (write verifier and
//    stateid epoch source).
//  - ProtocolStack: the v3/MOUNT engines, the v4.1/4.2 engine with its pseudo-fs and
//    state manager, the DRC and the per-object lock registry, all wired onto one RPC
//    dispatcher.  Members are declared in dependency order and live until run_server
//    returns; main.cpp only builds and tears this down.

#include <atomic>
#include <memory>
#include <optional>

#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "core/pseudofs.hpp"
#include "mountd/mount3.hpp"
#include "nfsv3/engine.hpp"
#include "nfsv4/engine.hpp"
#include "rpc/dispatch.hpp"
#include "rpc/drc.hpp"
#include "runtime/runtime.hpp"
#include "state/state_mgr.hpp"

namespace lnfs::server {

// Durable identity: export table, handle HMAC key (bound to the exports), boot epoch.
struct CoreState {
  std::unique_ptr<core::ExportTable> exports;
  core::FileHandleCodec key;
  uint64_t epoch = 0;
};

// Protocol engines and their shared state, wired onto one dispatcher.
struct ProtocolStack {
  rpc::Dispatcher dispatcher;
  core::ObjLockRegistry locks;
  rpc::Drc drc;
  nfsv3::Engine nfs3;
  mountd::Mount3 mount;
  // v4.1 stack: pseudo-fs namespace + session state + COMPOUND engine (enable_v4).
  core::PseudoFs pseudofs;
  state::StateMgr state;
  std::optional<nfsv4::Engine> nfs4;
  std::atomic<bool> lease_stop{false};

  // Builds the v3 side (engine + MOUNT registered on the dispatcher, DRC attached,
  // write verifier from the boot epoch) and the StateMgr, including the native
  // byte-range lock push-down hooks for exports whose backend has native_locks().
  ProtocolStack(const core::ServerConfig& cfg, CoreState& core);

  // Grace list + COMPOUND engine registration + the lease scanner coroutine
  // (design 07 §7.4: expiry → courtesy → conflict/timeout reclaim) on the last reactor.
  void enable_v4(const core::ServerConfig& cfg, CoreState& core, rt::Runtime& runtime);
};

}  // namespace lnfs::server
