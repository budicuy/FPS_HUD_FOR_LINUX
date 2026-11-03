#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace fpshud {

#if defined(__cpp_pack_indexing)

template <std::size_t N, typename... Types>
using PackType = Types...[N];

template <std::size_t N, typename... Args>
constexpr decltype(auto) pack_element(Args&&... args) noexcept {
    static_assert(N < sizeof...(Args), "pack_element index out of range");
    return static_cast<Args...[N]&&>(args...[N]);
}

#else

// Fallback for compilers without native C++26 pack indexing
template <std::size_t N, typename T0, typename... Rest>
struct PackTypeHelper {
    using type = typename PackTypeHelper<N - 1, Rest...>::type;
};

template <typename T0, typename... Rest>
struct PackTypeHelper<0, T0, Rest...> {
    using type = T0;
};

template <std::size_t N, typename... Types>
using PackType = typename PackTypeHelper<N, Types...>::type;

template <std::size_t N, typename T0, typename... Rest>
constexpr decltype(auto) pack_element(T0&& arg0, Rest&&... rest) noexcept {
    if constexpr (N == 0) {
        return std::forward<T0>(arg0);
    } else {
        return pack_element<N - 1>(std::forward<Rest>(rest)...);
    }
}

#endif

} // namespace fpshud
