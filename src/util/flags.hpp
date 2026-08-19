#pragma once
// Type-safe bit flags over an enum class (Caps etc., design 05 §5.2).

#include <type_traits>

namespace lnfs {

template <class E>
class Flags {
  static_assert(std::is_enum_v<E>);
  using U = std::underlying_type_t<E>;

 public:
  constexpr Flags() = default;
  constexpr Flags(E e) : v_(static_cast<U>(e)) {}  // NOLINT(google-explicit-constructor)
  constexpr bool has(E e) const { return (v_ & static_cast<U>(e)) == static_cast<U>(e); }
  constexpr bool any() const { return v_ != 0; }
  constexpr Flags& set(E e) {
    v_ |= static_cast<U>(e);
    return *this;
  }
  constexpr Flags& clear(E e) {
    v_ &= ~static_cast<U>(e);
    return *this;
  }
  constexpr Flags operator|(Flags o) const { return Flags(U(v_ | o.v_)); }
  constexpr Flags operator&(Flags o) const { return Flags(U(v_ & o.v_)); }
  constexpr Flags operator|(E e) const { return *this | Flags(e); }
  constexpr bool operator==(const Flags&) const = default;
  constexpr U raw() const { return v_; }

 private:
  constexpr explicit Flags(U v) : v_(v) {}
  U v_ = 0;
};

}  // namespace lnfs
