#pragma once

// High-performance zero-heap inplace_vector polyfill for Linux distributions
// lacking GCC 16+ or Clang 20+ with libstdc++ 16+.

#if defined(__has_include)
#  if __has_include(<inplace_vector>)
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic push
#      pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#    endif
#    include <inplace_vector>
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic pop
#    endif
#  endif
#endif

#if !defined(__cpp_lib_inplace_vector)
#include <cstddef>
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

    constexpr ~inplace_vector() {
        clear();
    }

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

    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return N; }
    [[nodiscard]] constexpr size_type max_size() const noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr reference operator[](size_type idx) noexcept { return data()[idx]; }
    constexpr const_reference operator[](size_type idx) const noexcept { return data()[idx]; }

    constexpr pointer data() noexcept { return reinterpret_cast<pointer>(storage_); }
    constexpr const_pointer data() const noexcept { return reinterpret_cast<const_pointer>(storage_); }

    constexpr iterator begin() noexcept { return data(); }
    constexpr const_iterator begin() const noexcept { return data(); }
    constexpr iterator end() noexcept { return data() + size_; }
    constexpr const_iterator end() const noexcept { return data() + size_; }

    constexpr void clear() noexcept {
        for (size_t i = 0; i < size_; ++i) {
            data()[i].~T();
        }
        size_ = 0;
    }

    constexpr pointer try_push_back(const T& val) {
        if (size_ >= N) return nullptr;
        new (static_cast<void*>(data() + size_)) T(val);
        ++size_;
        return data() + (size_ - 1);
    }

    constexpr pointer try_push_back(T&& val) {
        if (size_ >= N) return nullptr;
        new (static_cast<void*>(data() + size_)) T(std::move(val));
        ++size_;
        return data() + (size_ - 1);
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

    constexpr void resize(size_type count) {
        if (count > N) throw std::bad_alloc();
        while (size_ > count) {
            data()[--size_].~T();
        }
        while (size_ < count) {
            new (static_cast<void*>(data() + size_)) T();
            ++size_;
        }
    }

private:
    alignas(T) std::byte storage_[sizeof(T) * (N > 0 ? N : 1)];
    size_type size_ = 0;
};

} // namespace std
#define __cpp_lib_inplace_vector 202406L
#endif
