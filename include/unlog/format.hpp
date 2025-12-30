#pragma once

#include "types.hpp"

#include "fmt/base.h"
#include "fmt/format.h"

namespace un::log {
    using namespace std::literals;

    inline const auto DEFAULT_PATTERN = "[%H:%M:%S.%e] [%*] [%n:%l|%g:%#] >> %v"s;
    inline const auto DEFAULT_PATTERN_COLOR = "[%H:%M:%S.%e] [%*] [%n:%^%l%$|%g:%#] >> %v"s;

    // Types can opt-in to being fmt-formattable by ensuring they have a ::to_string() method defined
    template <typename T>
    concept to_string_formattable = T::to_string_formattable && requires(T a) {
        { a.to_string() } -> std::convertible_to<std::string_view>;
    };

    namespace detail {
        template <size_t N>
        struct string_literal {
            std::array<char, N> str;

            consteval string_literal(const char (&s)[N]) { std::ranges::copy(s, s + N, str.begin()); }
            consteval std::string_view sv() const { return {str.data(), N - 1}; }
        };

        template <string_literal Format>
        struct fmt_wrapper {
            consteval fmt_wrapper() = default;

            /// Calling on this object forwards all the values to fmt::format, using the format
            /// string as provided during type definition (via the "..."_format user-defined
            /// function).
            template <typename... T>
            constexpr auto operator()(T&&... args) && {
                return fmt::format(Format.sv(), std::forward<T>(args)...);
            }
        };
    }  //  namespace detail

    namespace literals {
        template <detail::string_literal Format>
        inline consteval auto operator""_format() {
            return detail::fmt_wrapper<Format>{};
        }

    }  // namespace literals

}  // namespace un::log

namespace fmt {
    template <un::log::to_string_formattable T>
    struct formatter<T, char> : formatter<std::string_view> {
        template <typename FormatContext>
        auto format(const T& val, FormatContext& ctx) const {
            return formatter<std::string_view>::format(val.to_string(), ctx);
        }
    };

    template <un::log::const_span_type T>
    struct formatter<T, char> : formatter<std::string_view> {
        template <typename FormatContext>
        auto format(const T& val, FormatContext& ctx) const {
            return formatter<std::string_view>::format(
                    std::string_view{reinterpret_cast<const char*>(val.data()), val.size()}, ctx);
        }
    };
}  // namespace fmt
