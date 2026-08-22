#pragma once
// NFSv4.1 engine (design 04 §4.5, phase-4 read-write scope): COMPOUND interpreter with
// CFH/SFH registers, stop-on-first-error semantics, response-size budgeting, session
// integration (SEQUENCE slots via state::StateMgr), the full open-state operation set
// (OPEN claim NULL/FH/PREVIOUS with create modes, CLOSE, OPEN_DOWNGRADE), stateid-
// checked IO (READ/WRITE/COMMIT/SETATTR) and the namespace ops (CREATE/REMOVE/RENAME/
// LINK).  Write verifier = boot epoch (shared with v3: one restart signal).
// minorversion 0 is permanently rejected (decision D5); unimplemented ops answer
// NOTSUPP, unknown opcodes answer OP_ILLEGAL.

#include "core/boot_epoch.hpp"
#include "core/config.hpp"
#include "core/file_handle.hpp"
#include "core/obj_lock.hpp"
#include "core/pseudofs.hpp"
#include "nfsv4/nfs4_types.hpp"
#include "rpc/dispatch.hpp"
#include "state/state_mgr.hpp"

namespace lnfs::nfsv4 {

class Engine {
 public:
  Engine(core::ExportTable& exports, core::FileHandleCodec& handles,
         core::ObjLockRegistry& locks, core::PseudoFs& pseudo, state::StateMgr& state)
      : exports_(exports), handles_(handles), locks_(locks), pseudo_(pseudo),
        state_(state), write_verf_(core::verifier_from_epoch(state.config().boot_epoch)) {}

  void register_with(rpc::Dispatcher& dispatcher);
  rt::Task<void> dispatch(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);

 private:
  // One filehandle register (CFH or SFH): raw bytes, validated on store.
  using FhBytes = std::vector<std::byte>;

  // Resolution of a filehandle into either a pseudo node or an export object.
  struct Resolved {
    core::PseudoFs::Node* node = nullptr;  // pseudo (fsid 0)
    core::ExportEntry* exp = nullptr;      // export side
    backend::ObjPtr obj;
    backend::ObjId oid;
    bool pseudo() const { return node != nullptr; }
  };

  struct Ctx {
    transport::ConnCtx& conn;
    const rpc::Cred& cred;
    FhBytes cfh, sfh;
    bool session = false;
    state::SessionId sessionid{};
    uint32_t slotid = 0, seqid = 0;
    bool cachethis = false;
    uint64_t clientid = 0;
    size_t max_response = 1u << 20;  // effective reply budget for this compound
    // Current stateid (RFC 8881 §16.2.3.1.2): set by OPEN/OPEN_DOWNGRADE/CLOSE,
    // consumed by ops given {seqid=1, other=0}, saved/restored with the filehandle,
    // cleared by ops that replace the current filehandle.
    Stateid current_sid{};
    bool current_valid = false;
    Stateid saved_sid{};
    bool saved_valid = false;
  };
  // Substitutes the current stateid for its placeholder; BAD_STATEID if none is set.
  static uint32_t resolve_current(const Ctx& ctx, Stateid& sid);

  rt::Task<void> compound(transport::ConnCtx&, rpc::RpcCall&, const rpc::Cred&);
  rt::Task<Result<Resolved>> resolve(const FhBytes& fh, const sockaddr_storage& peer);

  // Each op encodes {opcode, status, body} into enc and returns the status.
  rt::Task<uint32_t> exec_op(Ctx& ctx, uint32_t opcode, xdr::XdrDec& dec,
                             xdr::XdrEnc& enc);

  rt::Task<uint32_t> op_putrootfh(Ctx&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_putfh(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  uint32_t op_getfh(Ctx&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_lookup(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_lookupp(Ctx&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_getattr(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_access(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_readlink(Ctx&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_read(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_readdir(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_open(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_close(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_open_downgrade(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_write(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_commit(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_setattr(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_create(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_remove(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_rename(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_link(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_verify(Ctx&, xdr::XdrDec&, xdr::XdrEnc&, bool nverify);
  rt::Task<uint32_t> op_lock(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_lockt(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_locku(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_secinfo(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<Result<Resolved>> resolve_lock_target(Ctx&, uint32_t* status);
  rt::Task<uint32_t> op_secinfo_no_name(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_free_stateid(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_test_stateid(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_reclaim_complete(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);

  // Sessionless (solo-compound) operations.
  rt::Task<uint32_t> op_exchange_id(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_create_session(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_destroy_session(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_destroy_clientid(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);
  rt::Task<uint32_t> op_bind_conn(Ctx&, xdr::XdrDec&, xdr::XdrEnc&);

  // Shared helpers.
  rt::Task<uint32_t> attr_reply(Ctx&, const Resolved&, const Bitmap& wanted,
                                xdr::XdrEnc&);  // GETATTR tail
  FhBytes pseudo_fh(const core::PseudoFs::Node& node) const;
  FhBytes export_fh(const core::ExportEntry& exp, const backend::ObjId& oid) const;

  core::ExportTable& exports_;
  core::FileHandleCodec& handles_;
  core::ObjLockRegistry& locks_;
  core::PseudoFs& pseudo_;
  state::StateMgr& state_;
  core::WriteVerf write_verf_;
};

}  // namespace lnfs::nfsv4
