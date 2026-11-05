#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace fpshud {

template <std::size_t N, typename... Types>
using PackType = Types...[N];

template <std::size_t N, typename... Args>
constexpr decltype(auto) pack_element(Args&&... args) noexcept {
  static_assert(N < sizeof...(Args), "pack_element index out of range");
  if constexpr (std::is_rvalue_reference_v<decltype(args...[N])>) {
    return static_cast<PackType<N, Args...>>(std::move(args...[N]));
  } else {
    return static_cast<PackType<N, Args...>&&>(args...[N]);
  }
}

}  // namespace fpshud
