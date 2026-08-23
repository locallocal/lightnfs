#pragma once
// SmallVec<T, N>: vector with inline storage for the first N elements (design 05/07 use it
// for gid lists, session refs, dir pages).

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace lnfs {

template <class T, size_t N>
class SmallVec {
  static_assert(N > 0, "inline capacity must be nonzero (cap_ starts at N)");

 public:
  SmallVec() = default;
  SmallVec(const SmallVec& o) { append_from(o); }
  SmallVec(SmallVec&& o) noexcept {
    if (o.heap_) {
      heap_ = o.heap_;
      cap_ = o.cap_;
      size_ = o.size_;
      o.heap_ = nullptr;
      o.cap_ = N;
      o.size_ = 0;
    } else {
      for (size_t i = 0; i < o.size_; ++i) push_back(std::move(o.data()[i]));
      o.clear();
    }
  }
  SmallVec& operator=(const SmallVec& o) {
    if (this != &o) {
      clear();
      append_from(o);
    }
    return *this;
  }
  SmallVec& operator=(SmallVec&& o) noexcept {
    if (this != &o) {
      this->~SmallVec();
      new (this) SmallVec(std::move(o));
    }
    return *this;
  }
  ~SmallVec() {
    clear();
    ::operator delete(heap_);
  }

  T* data() { return heap_ ? heap_ : reinterpret_cast<T*>(inline_); }
  const T* data() const { return heap_ ? heap_ : reinterpret_cast<const T*>(inline_); }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  T& operator[](size_t i) {
    assert(i < size_);
    return data()[i];
  }
  const T& operator[](size_t i) const {
    assert(i < size_);
    return data()[i];
  }
  T* begin() { return data(); }
  T* end() { return data() + size_; }
  const T* begin() const { return data(); }
  const T* end() const { return data() + size_; }
  T& back() { return data()[size_ - 1]; }

  void push_back(T v) { emplace_back(std::move(v)); }
  template <class... A>
  T& emplace_back(A&&... a) {
    if (size_ == cap_) grow(cap_ * 2);
    T* p = new (data() + size_) T(std::forward<A>(a)...);
    ++size_;
    return *p;
  }
  void clear() {
    for (size_t i = 0; i < size_; ++i) data()[i].~T();
    size_ = 0;
  }

 private:
  void grow(size_t ncap) {
    if (ncap == 0) ncap = 1;  // unreachable (cap_ >= N >= 1); keeps GCC15 -Warray-bounds
                              // from assuming a zero-size allocation below
    T* nh = static_cast<T*>(::operator new(ncap * sizeof(T)));
    T* old = data();
    for (size_t i = 0; i < size_; ++i) {
      new (nh + i) T(std::move(old[i]));
      old[i].~T();
    }
    ::operator delete(heap_);
    heap_ = nh;
    cap_ = ncap;
  }
  void append_from(const SmallVec& o) {
    for (size_t i = 0; i < o.size_; ++i) emplace_back(o.data()[i]);
  }

  alignas(T) unsigned char inline_[sizeof(T) * N];
  T* heap_ = nullptr;
  size_t cap_ = N;
  size_t size_ = 0;
};

}  // namespace lnfs
