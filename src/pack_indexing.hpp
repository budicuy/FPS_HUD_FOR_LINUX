#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace fpshud {

#if defined(__cpp_pack_indexing)

namespace detail {

template <std::size_t N, typename... Types>
struct PackTypeChecked {
    static_assert(N < sizeof...(Types), "PackType index out of range");
    using type = Types...[N];
};

} // namespace detail

template <std::size_t N, typename... Types>
using PackType = typename detail::PackTypeChecked<N, Types...>::type;

template <std::size_t N, typename... Args>
constexpr auto pack_element(Args&&... args)
    noexcept(std::is_nothrow_constructible_v<PackType<N, Args...>, PackType<N, Args...>&&>)
    -> PackType<N, Args...> {
    static_assert(N < sizeof...(Args), "pack_element index out of range");
    return static_cast<Args...[N]&&>(args...[N]);
}

#else

// Fallback for compilers without native C++26 pack indexing
namespace detail {

template <std::size_t N, typename... Types>
struct PackTypeHelper;

template <std::size_t N, typename T0, typename... Rest>
struct PackTypeHelper<N, T0, Rest...> {
    using type = typename PackTypeHelper<N - 1, Rest...>::type;
};

template <typename T0, typename... Rest>
struct PackTypeHelper<0, T0, Rest...> {
    using type = T0;
};

template <std::size_t N, typename... Types>
struct PackTypeChecked {
    static_assert(N < sizeof...(Types), "PackType index out of range");
    using type = typename PackTypeHelper<N, Types...>::type;
};

template <std::size_t N, typename T0, typename... Rest>
constexpr decltype(auto) pack_element_impl(T0&& arg0, Rest&&... rest) noexcept {
    if constexpr (N == 0) {
        return std::forward<T0>(arg0);
    } else {
        return pack_element_impl<N - 1>(std::forward<Rest>(rest)...);
    }
}

} // namespace detail

template <std::size_t N, typename... Types>
using PackType = typename detail::PackTypeChecked<N, Types...>::type;

template <std::size_t N, typename... Args>
constexpr auto pack_element(Args&&... args)
    noexcept(std::is_nothrow_constructible_v<PackType<N, Args...>, PackType<N, Args...>&&>)
    -> PackType<N, Args...> {
    static_assert(N < sizeof...(Args), "pack_element index out of range");
    return detail::pack_element_impl<N>(std::forward<Args>(args)...);
}

#endif

} // namespace fpshud
