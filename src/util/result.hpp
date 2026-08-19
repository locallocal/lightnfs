#pragma once
// Result<T> = expected<T, Errno> (design decision D6). Self-contained minimal implementation
// with the subset of the std::expected API we use; swappable for tl/std::expected later.

#include <cassert>
#include <new>
#include <utility>

#include "util/errno.hpp"

namespace lnfs {

struct Unexpected {
  Errno e;
};
constexpr Unexpected Err(Errno e) { return Unexpected{e}; }

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T v) : ok_(true) { new (&val_) T(std::move(v)); }  // NOLINT(google-explicit-constructor)
  Result(Unexpected u) : ok_(false), err_(u.e) {}           // NOLINT(google-explicit-constructor)
  Result(Result&& o) noexcept : ok_(o.ok_) {
    if (ok_) new (&val_) T(std::move(o.val_));
    else err_ = o.err_;
  }
  Result(const Result& o) : ok_(o.ok_) {
    if (ok_) new (&val_) T(o.val_);
    else err_ = o.err_;
  }
  Result& operator=(Result&& o) noexcept {
    if (this != &o) {
      this->~Result();
      new (this) Result(std::move(o));
    }
    return *this;
  }
  ~Result() {
    if (ok_) val_.~T();
  }

  bool has_value() const { return ok_; }
  explicit operator bool() const { return ok_; }
  T& value() & {
    assert(ok_);
    return val_;
  }
  T&& value() && {
    assert(ok_);
    return std::move(val_);
  }
  const T& value() const& {
    assert(ok_);
    return val_;
  }
  Errno error() const {
    assert(!ok_);
    return err_;
  }
  T& operator*() & { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  const T& operator*() const& { return value(); }
  T* operator->() {
    assert(ok_);
    return &val_;
  }
  const T* operator->() const {
    assert(ok_);
    return &val_;
  }
  T value_or(T alt) const& { return ok_ ? val_ : std::move(alt); }

 private:
  bool ok_;
  union {
    T val_;
  };
  Errno err_{Errno::kOk};
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() : ok_(true) {}
  Result(Unexpected u) : ok_(false), err_(u.e) {}  // NOLINT(google-explicit-constructor)
  bool has_value() const { return ok_; }
  explicit operator bool() const { return ok_; }
  void value() const { assert(ok_); }
  Errno error() const {
    assert(!ok_);
    return err_;
  }

 private:
  bool ok_;
  Errno err_{Errno::kOk};
};

// Propagate-on-error helper: LNFS_TRY(expr) yields the value or returns the error.
#define LNFS_TRY(expr)                            \
  ({                                              \
    auto _r = (expr);                             \
    if (!_r) return ::lnfs::Err(_r.error());      \
    std::move(_r).value();                        \
  })

// co_return flavor for coroutines.
#define LNFS_CO_TRY(expr)                         \
  ({                                              \
    auto _r = (expr);                             \
    if (!_r) co_return ::lnfs::Err(_r.error());   \
    std::move(_r).value();                        \
  })

}  // namespace lnfs
