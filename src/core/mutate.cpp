#include "core/mutate.hpp"

#include <utility>

namespace lnfs::core {

rt::Task<std::optional<backend::Attr>> sample_attr(const backend::ObjPtr& obj) {
  auto attr = co_await obj->getattr();
  co_return attr ? std::optional<backend::Attr>(*attr) : std::nullopt;
}

MutateGuard::Verdict MutateGuard::precheck(std::initializer_list<std::string_view> names) const {
  if (exp_.readonly) return {Verdict::kReadonly};
  size_t index = 0;
  for (std::string_view name : names) {
    if (NameCheck c = check_component(name); c != NameCheck::kOk)
      return {Verdict::kBadName, c, index};
    ++index;
  }
  return {};
}

void MutateGuard::squash() {
  mapped_ = exports_.squash_cred(rpc_cred_, exp_);
  cred_ = mapped_.view();
}

rt::Task<void> MutateGuard::enter(Target a) {
  squash();
  a_ = std::move(a);
  b_ = Target{};
  b_.sample = false;
  same_ = true;  // second() aliases first()
  lock_a_ = locks_.get(exp_.fsid, a_.oid);
  held_a_ = co_await lock_a_->lock();
  if (a_.sample) sample_a_.before = co_await sample_attr(a_.obj);
}

rt::Task<void> MutateGuard::enter(Target a, Target b) {
  squash();
  a_ = std::move(a);
  b_ = std::move(b);
  lock_a_ = locks_.get(exp_.fsid, a_.oid);
  lock_b_ = locks_.get(exp_.fsid, b_.oid);
  same_ = lock_a_.get() == lock_b_.get();
  if (same_) {
    lock_b_.reset();
    held_a_ = co_await lock_a_->lock();
    if (a_.sample || b_.sample) sample_a_.before = co_await sample_attr(a_.obj);
    co_return;
  }
  // Deterministic order by ObjId so two concurrent two-directory ops cannot deadlock.
  bool swap = b_.oid < a_.oid;
  auto& outer = swap ? lock_b_ : lock_a_;
  auto& inner = swap ? lock_a_ : lock_b_;
  auto& outer_held = swap ? held_b_ : held_a_;
  auto& inner_held = swap ? held_a_ : held_b_;
  outer_held = co_await outer->lock();
  inner_held = co_await inner->lock();
  if (a_.sample) sample_a_.before = co_await sample_attr(a_.obj);
  if (b_.sample) sample_b_.before = co_await sample_attr(b_.obj);
}

rt::Task<void> MutateGuard::finish() {
  if (same_) {
    if (a_.sample || b_.sample) sample_a_.after = co_await sample_attr(a_.obj);
    co_return;
  }
  if (a_.sample) sample_a_.after = co_await sample_attr(a_.obj);
  if (b_.sample) sample_b_.after = co_await sample_attr(b_.obj);
}

}  // namespace lnfs::core
