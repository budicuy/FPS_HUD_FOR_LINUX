#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace fpshud {

template <std::size_t N, typename... Types>
using PackType = Types...[N];

template <std::size_t N, typename... Args>
constexpr PackType<N, Args...> pack_element(Args&&... args) noexcept(
    std::is_nothrow_constructible_v<PackType<N, Args...>, PackType<N, Args...>&&>) {
  static_assert(N < sizeof...(Args), "pack_element index out of range");
  return static_cast<Args...[N]&&>(args...[N]);
}

}  // namespace fpshud
