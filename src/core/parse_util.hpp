#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace fpshud::parse {

inline constexpr bool is_whitespace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline constexpr std::string_view trim_leading_whitespace(std::string_view sv) noexcept {
  while (!sv.empty() && is_whitespace(sv.front())) {
    sv.remove_prefix(1);
  }
  return sv;
}

inline constexpr std::string_view trim_trailing_whitespace(std::string_view sv) noexcept {
  while (!sv.empty() && is_whitespace(sv.back())) {
    sv.remove_suffix(1);
  }
  return sv;
}

inline constexpr std::string_view trim_whitespace(std::string_view sv) noexcept {
  return trim_trailing_whitespace(trim_leading_whitespace(sv));
}

template <typename IntT>
[[nodiscard]] bool parse_int_prefix(std::string_view sv, IntT& out,
                                    std::string_view* remainder = nullptr, int base = 10) noexcept {
  static_assert(std::is_integral_v<IntT>, "parse_int requires an integral type");
  static_assert(!std::is_same_v<IntT, bool>, "parse_int does not support bool");

  sv = trim_leading_whitespace(sv);
  if (sv.empty()) {
    return false;
  }

  // Handle optional leading '+'
  if (sv.front() == '+') {
    sv.remove_prefix(1);
    if (sv.empty()) {
      return false;
    }
  }

  // Handle "0x" / "0X" prefix for base 16
  if (base == 16) {
    if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
      sv.remove_prefix(2);
      if (sv.empty()) {
        return false;
      }
    }
  }

  // Reject '-' on unsigned types cleanly
  if constexpr (std::is_unsigned_v<IntT>) {
    if (sv.front() == '-') {
      return false;
    }
  }

  const char* first = sv.data();
  const char* last = sv.data() + sv.size();

  if constexpr (sizeof(IntT) < sizeof(int)) {
    using WideT = std::conditional_t<std::is_signed_v<IntT>, int, unsigned int>;
    WideT temp{};
    auto [ptr, ec] = std::from_chars(first, last, temp, base);
    if (ec != std::errc{} || ptr == first) {
      return false;
    }
    if (temp < std::numeric_limits<IntT>::min() || temp > std::numeric_limits<IntT>::max()) {
      return false;
    }
    out = static_cast<IntT>(temp);
    if (remainder) {
      *remainder = std::string_view(ptr, last - ptr);
    }
    return true;
  } else {
    auto [ptr, ec] = std::from_chars(first, last, out, base);
    if (ec != std::errc{} || ptr == first) {
      return false;
    }
    if (remainder) {
      *remainder = std::string_view(ptr, last - ptr);
    }
    return true;
  }
}

template <typename IntT>
[[nodiscard]] bool parse_int(std::string_view sv, IntT& out, int base = 10) noexcept {
  return parse_int_prefix(sv, out, nullptr, base);
}

template <typename IntT>
[[nodiscard]] bool parse_int_exact(std::string_view sv, IntT& out, int base = 10) noexcept {
  std::string_view rem;
  if (!parse_int_prefix(sv, out, &rem, base)) {
    return false;
  }
  rem = trim_whitespace(rem);
  return rem.empty();
}

template <typename IntT>
[[nodiscard]] IntT parse_int_or(std::string_view sv, IntT fallback, int base = 10) noexcept {
  IntT val{};
  if (parse_int(sv, val, base)) {
    return val;
  }
  return fallback;
}

[[nodiscard]] inline bool parse_float_prefix(std::string_view sv, float& out,
                                             std::string_view* remainder = nullptr) noexcept {
  sv = trim_leading_whitespace(sv);
  if (sv.empty()) {
    return false;
  }

  // Handle optional leading '+'
  if (sv.front() == '+') {
    sv.remove_prefix(1);
    if (sv.empty()) {
      return false;
    }
  }

  const char* first = sv.data();
  const char* last = sv.data() + sv.size();

  auto [ptr, ec] = std::from_chars(first, last, out);
  if (ec != std::errc{} || ptr == first) {
    return false;
  }
  if (remainder) {
    *remainder = std::string_view(ptr, last - ptr);
  }
  return true;
}

[[nodiscard]] inline bool parse_float(std::string_view sv, float& out) noexcept {
  return parse_float_prefix(sv, out, nullptr);
}

[[nodiscard]] inline bool parse_float_exact(std::string_view sv, float& out) noexcept {
  std::string_view rem;
  if (!parse_float_prefix(sv, out, &rem)) {
    return false;
  }
  rem = trim_whitespace(rem);
  return rem.empty();
}

[[nodiscard]] inline float parse_float_or(std::string_view sv, float fallback) noexcept {
  float val{};
  if (parse_float(sv, val)) {
    return val;
  }
  return fallback;
}

}  // namespace fpshud::parse
