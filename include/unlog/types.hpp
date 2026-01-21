#pragma once

#include "utils.hpp"

#include <span>

namespace un::log {

    template <typename T>
    concept basic_char = sizeof(T) == 1 && !std::same_as<T, bool> && (std::integral<T> || std::same_as<T, std::byte>);

    using cspan = std::span<const char>;
    using uspan = std::span<const unsigned char>;
    using bspan = std::span<const std::byte>;

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept const_span_type = std::same_as<U, cspan> || std::same_as<U, uspan> || std::same_as<U, bspan>;

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept const_span_convertible =
            std::convertible_to<U, cspan> || std::convertible_to<U, uspan> || std::convertible_to<U, bspan>;

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept char_view_type = std::convertible_to<U, cspan> || std::convertible_to<U, std::string_view>;

    inline namespace operators {
        template <const_span_type T, const_span_convertible R>
            requires std::same_as<typename T::value_type, typename R::value_type>
        inline constexpr bool operator==(T lhs, const R& rhs) {
            return std::ranges::equal(lhs, rhs);
        }

        template <const_span_type T, const_span_convertible R>
            requires std::same_as<typename T::value_type, typename R::value_type>
        inline constexpr auto operator<=>(T lhs, const R& rhs) {
            return std::lexicographical_compare_three_way(
                    lhs.begin(), lhs.end(), std::ranges::begin(rhs), std::ranges::end(rhs));
        }
    }  // namespace operators

    namespace detail {
        template <basic_char T, size_t N>
        struct span_literal {
            consteval span_literal(const char (&s)[N]) {
                for (size_t i = 0; i < N; ++i)
                    arr[i] = std::bit_cast<T>(s[i]);
            }

            static constexpr size_t SIZE{N - 1};
            std::array<T, N> arr;

            constexpr auto view() const { return std::string_view{reinterpret_cast<const char*>(arr.data()), SIZE}; }
            constexpr auto span() const { return std::span<const T, SIZE>{arr.begin(), SIZE}; }

            operator std::span<const T, SIZE>() { return span(); }

            std::string to_string() const { return std::string{reinterpret_cast<const char*>(arr.data()), SIZE}; }
            static constexpr auto to_string_formattable = true;
        };

    }  // namespace detail

    template <size_t N>
    struct sp_literal : detail::span_literal<char, N> {
        consteval sp_literal(const char (&s)[N]) : detail::span_literal<char, N>{s} {}
    };

    template <size_t N>
    struct usp_literal : detail::span_literal<unsigned char, N> {
        consteval usp_literal(const char (&s)[N]) : detail::span_literal<unsigned char, N>{s} {}
    };

    template <size_t N>
    struct bsp_literal : detail::span_literal<std::byte, N> {
        consteval bsp_literal(const char (&s)[N]) : detail::span_literal<std::byte, N>{s} {}
    };

    namespace literals {
        template <sp_literal Csp>
        inline consteval auto operator""_sp() {
            return Csp.span();
        }

        template <usp_literal Usp>
        inline consteval auto operator""_usp() {
            return Usp.span();
        }

        template <bsp_literal Bsp>
        inline consteval auto operator""_bsp() {
            return Bsp.span();
        }

    }  // namespace literals
}  // namespace un::log

namespace std {

    template <>
    struct hash<un::log::cspan> {
        size_t operator()(const un::log::cspan& sp) const noexcept {
            return hash<string_view>{}({sp.data(), sp.size()});
        }
    };

}  // namespace std
