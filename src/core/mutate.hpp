#pragma once
// Mutation guard (plan doc 10 §6.1; the "core::mutate" template of design 04 §4.2).
// The one place that spells out the sequence every write procedure follows once its
// handle(s) have resolved:
//
//   1. readonly precheck          (export flag)
//   2. name precheck              (component discipline, core/names.hpp)
//   3. credential squash
//   4. exclusive object lock(s)   (two targets: ObjId order, design 04 §4.2)
//   5. before sample
//      -- backend op, issued by the engine --
//   6. after sample
//
// v3 turns the samples into WCC, v4 into change_info4; the engines only encode.
// Locks are held until the guard is destroyed, i.e. through reply encoding.

#include <initializer_list>
#include <optional>
#include <string_view>

#include "backend/api.hpp"
#include "core/config.hpp"
#include "core/names.hpp"
#include "core/obj_lock.hpp"

namespace lnfs::core {

// Pre/post attribute samples of one object around a mutation (§6.3): the shared
// source for v3 wcc_data and v4 change_info4.  A missing sample (getattr failed)
// encodes as "no attributes" in v3 and as change 0 in v4.
struct ChangeSample {
  std::optional<backend::Attr> before, after;
  uint64_t change_before() const { return before ? before->change : 0; }
  uint64_t change_after() const { return after ? after->change : 0; }
};

// One getattr, absent on failure — the primitive every sample uses.
rt::Task<std::optional<backend::Attr>> sample_attr(const backend::ObjPtr& obj);

class MutateGuard {
 public:
  struct Target {
    backend::ObjPtr obj;
    backend::ObjId oid;
    bool sample = true;  // false: lock only (ops whose reply carries no change data)
  };

  // Outcome of steps 1-2, in evaluation order.
  struct Verdict {
    enum Kind : uint8_t { kOk, kReadonly, kBadName } kind = kOk;
    NameCheck name = NameCheck::kOk;  // set for kBadName
    size_t name_index = 0;            // which of the checked names failed
    explicit operator bool() const { return kind == kOk; }
  };

  MutateGuard(ObjLockRegistry& locks, const ExportTable& exports, const ExportEntry& exp,
              const rpc::Cred& rpc_cred)
      : locks_(locks), exports_(exports), exp_(exp), rpc_cred_(rpc_cred) {}
  MutateGuard(const MutateGuard&) = delete;
  MutateGuard& operator=(const MutateGuard&) = delete;

  // Steps 1-2: the export's readonly flag first, then each name the op will create or
  // remove ("." / ".." are never acceptable here).
  Verdict precheck(std::initializer_list<std::string_view> names) const;

  // Steps 3-5.  The two-target form takes the locks in ObjId order; when both targets
  // are the same object it takes one lock and second() aliases first().
  rt::Task<void> enter(Target a);
  rt::Task<void> enter(Target a, Target b);
  // Step 6: after samples for every target that asked to be sampled.
  rt::Task<void> finish();

  const backend::Cred& cred() const { return cred_; }
  const ChangeSample& first() const { return sample_a_; }
  const ChangeSample& second() const { return same_ ? sample_a_ : sample_b_; }
  bool same_object() const { return same_; }
  const ExportEntry& exp() const { return exp_; }
  uint32_t fsid() const { return exp_.fsid; }

 private:
  void squash();

  ObjLockRegistry& locks_;
  const ExportTable& exports_;
  const ExportEntry& exp_;
  const rpc::Cred& rpc_cred_;
  MappedCred mapped_;
  backend::Cred cred_;
  Target a_{}, b_{};
  bool same_ = false;
  std::shared_ptr<rt::AsyncSharedMutex> lock_a_, lock_b_;
  rt::AsyncSharedMutex::Lock held_a_, held_b_;
  ChangeSample sample_a_, sample_b_;
};

}  // namespace lnfs::core
