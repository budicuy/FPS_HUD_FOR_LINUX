#pragma once

// High-performance zero-heap inplace_vector polyfill for Linux distributions
// lacking GCC 16+ or Clang 20+ with libstdc++ 16+.

#if !defined(FPSHUD_FORCE_INPLACE_VECTOR_POLYFILL) && defined(__has_include)
#if __has_include(<inplace_vector>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <inplace_vector>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
#endif

#if !defined(__cpp_lib_inplace_vector) || defined(FPSHUD_FORCE_INPLACE_VECTOR_POLYFILL)
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace std {

template <typename T, size_t N>
class inplace_vector {
public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = T*;
  using const_iterator = const T*;

  constexpr inplace_vector() noexcept : size_(0) {}

  constexpr ~inplace_vector() { clear(); }

  constexpr inplace_vector(const inplace_vector& other) : size_(0) {
    for (size_t i = 0; i < other.size_; ++i) {
      push_back(other[i]);
    }
  }

  constexpr inplace_vector(inplace_vector&& other) noexcept : size_(0) {
    for (size_t i = 0; i < other.size_; ++i) {
      push_back(std::move(other[i]));
    }
    other.clear();
  }

  constexpr inplace_vector(std::initializer_list<T> il) : size_(0) {
    if (il.size() > N) {
      throw std::bad_alloc();
    }
    for (const auto& item : il) {
      push_back(item);
    }
  }

  constexpr inplace_vector& operator=(const inplace_vector& other) {
    if (this != &other) {
      clear();
      for (size_t i = 0; i < other.size_; ++i) {
        push_back(other[i]);
      }
    }
    return *this;
  }

  constexpr inplace_vector& operator=(inplace_vector&& other) noexcept {
    if (this != &other) {
      clear();
      for (size_t i = 0; i < other.size_; ++i) {
        push_back(std::move(other[i]));
      }
      other.clear();
    }
    return *this;
  }

  constexpr inplace_vector& operator=(std::initializer_list<T> il) {
    if (il.size() > N) {
      throw std::bad_alloc();
    }
    clear();
    for (const auto& item : il) {
      push_back(item);
    }
    return *this;
  }

  [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
  [[nodiscard]] constexpr size_type capacity() const noexcept { return N; }
  [[nodiscard]] constexpr size_type max_size() const noexcept { return N; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  constexpr reference operator[](size_type idx) noexcept { return data()[idx]; }
  constexpr const_reference operator[](size_type idx) const noexcept { return data()[idx]; }

  constexpr pointer data() noexcept { return reinterpret_cast<pointer>(storage_); }
  constexpr const_pointer data() const noexcept {
    return reinterpret_cast<const_pointer>(storage_);
  }

  constexpr iterator begin() noexcept { return data(); }
  constexpr const_iterator begin() const noexcept { return data(); }
  constexpr iterator end() noexcept { return data() + size_; }
  constexpr const_iterator end() const noexcept { return data() + size_; }

  constexpr void clear() noexcept {
    while (size_ > 0) {
      --size_;
      std::destroy_at(data() + size_);
    }
  }

  [[nodiscard]] constexpr reference front() noexcept { return data()[0]; }
  [[nodiscard]] constexpr const_reference front() const noexcept { return data()[0]; }
  [[nodiscard]] constexpr reference back() noexcept { return data()[size_ - 1]; }
  [[nodiscard]] constexpr const_reference back() const noexcept { return data()[size_ - 1]; }

  constexpr void pop_back() noexcept {
    if (size_ > 0) {
      --size_;
      std::destroy_at(data() + size_);
    }
  }

  template <typename... Args>
  constexpr pointer try_emplace_back(Args&&... args) {
    if (size_ >= N) return nullptr;
    pointer p = std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return p;
  }

  template <typename... Args>
  constexpr reference emplace_back(Args&&... args) {
    pointer p = try_emplace_back(std::forward<Args>(args)...);
    if (!p) throw std::bad_alloc();
    return *p;
  }

  constexpr pointer try_push_back(const T& val) {
    if (size_ >= N) return nullptr;
    pointer p = std::construct_at(data() + size_, val);
    ++size_;
    return p;
  }

  constexpr pointer try_push_back(T&& val) {
    if (size_ >= N) return nullptr;
    pointer p = std::construct_at(data() + size_, std::move(val));
    ++size_;
    return p;
  }

  constexpr reference push_back(const T& val) {
    pointer p = try_push_back(val);
    if (!p) throw std::bad_alloc();
    return *p;
  }

  constexpr reference push_back(T&& val) {
    pointer p = try_push_back(std::move(val));
    if (!p) throw std::bad_alloc();
    return *p;
  }

  constexpr iterator erase(const_iterator pos) {
    iterator non_const_pos = data() + (pos - begin());
    if (non_const_pos < end()) {
      std::move(non_const_pos + 1, end(), non_const_pos);
      data()[--size_].~T();
    }
    return non_const_pos;
  }

  constexpr iterator erase(const_iterator first, const_iterator last) {
    difference_type count = last - first;
    iterator non_const_first = data() + (first - begin());
    if (count > 0) {
      iterator non_const_last = data() + (last - begin());
      std::move(non_const_last, end(), non_const_first);
      for (size_type i = 0; i < static_cast<size_type>(count); ++i) {
        data()[--size_].~T();
      }
    }
    return non_const_first;
  }

  constexpr void resize(size_type count) {
    if (count > N) throw std::bad_alloc();
    while (size_ > count) {
      --size_;
      std::destroy_at(data() + size_);
    }
    while (size_ < count) {
      std::construct_at(data() + size_);
      ++size_;
    }
  }

private:
  alignas(T) std::byte storage_[sizeof(T) * (N > 0 ? N : 1)];
  size_type size_ = 0;
};

template <typename T, size_t N, typename U = T>
constexpr typename inplace_vector<T, N>::size_type erase(inplace_vector<T, N>& c, const U& value) {
  auto it = std::remove(c.begin(), c.end(), value);
  auto r = std::distance(it, c.end());
  c.erase(it, c.end());
  return static_cast<typename inplace_vector<T, N>::size_type>(r);
}

template <typename T, size_t N, typename Pred>
constexpr typename inplace_vector<T, N>::size_type erase_if(inplace_vector<T, N>& c, Pred pred) {
  auto it = std::remove_if(c.begin(), c.end(), pred);
  auto r = std::distance(it, c.end());
  c.erase(it, c.end());
  return static_cast<typename inplace_vector<T, N>::size_type>(r);
}

}  // namespace std
#if !defined(__cpp_lib_inplace_vector)
#define __cpp_lib_inplace_vector 202406L
#endif
#endif
