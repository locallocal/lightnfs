#pragma once
// In-process stand-in for the Lustre kernel client (backend/llapi.hpp) so
// tests/test_lustre.cpp runs the whole Lustre backend on a plain directory: FIDs are
// derived from (inode, birth time) and pinned by an O_PATH descriptor so that
// open_by_fid works without .lustre/fid (reopen through /proc — what the real
// kernel does semantically: a FID resolves to the inode wherever it was renamed to,
// and to ENOENT once it is gone); HSM state and stripe size are test knobs.

#include <cstdint>
#include <string>
#include <vector>

#include "backend/llapi.hpp"

namespace lnfs::testing {

struct FakeLlapi {
  static const backend::llapi::Ops* ops();
  // Drops every pin and all HSM state; restores the default knobs.
  static void reset();

  // The FID the fake assigns to the object at `path` (also pins it).
  static backend::llapi::Fid fid_of_path(const std::string& path);
  // HSM state bits (llapi::kHs*) reported for `fid`; unset = 0 (not managed).
  static void set_hsm_states(const backend::llapi::Fid& fid, uint32_t states);
  static uint32_t hsm_states(const backend::llapi::Fid& fid);
  // RESTORE requests seen so far, in order.
  static std::vector<backend::llapi::Fid> restore_requests();
  // When set, a RESTORE request clears kHsReleased immediately (a fast coordinator);
  // otherwise it only marks the restore as in progress until the test clears the bit.
  static void set_auto_restore(bool on);
  // hsm_state() answers ENOTTY (a client without HSM) when false.
  static void set_hsm_supported(bool on);
  // Default stripe size reported for every object; 0 = ENODATA (no layout set).
  static void set_stripe_size(uint32_t bytes);
  // is_lustre() answer (default true).
  static void set_lustre(bool on);
  // Live pinned descriptors (leak accounting).
  static int pinned();
};

}  // namespace lnfs::testing
