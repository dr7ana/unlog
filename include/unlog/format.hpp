#pragma once

#include "types.hpp"

#include "fmt/base.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "fmt/std.h"

#include <array>
#include <string_view>

namespace un::log::backend {
    enum class time_requirements : uint8_t {
        none = 0,
        wall_clock = 1 << 0,
        elapsed = 1 << 1,
        wall_clock_elapsed = (1 << 0) | (1 << 1),
    };

    [[nodiscard]] constexpr time_requirements operator|(time_requirements lhs, time_requirements rhs) noexcept {
        return static_cast<time_requirements>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    }

    constexpr time_requirements& operator|=(time_requirements& lhs, time_requirements rhs) noexcept {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr bool requires_wall_clock(time_requirements requirements) noexcept {
        return (static_cast<uint8_t>(requirements) & static_cast<uint8_t>(time_requirements::wall_clock)) != 0;
    }

    [[nodiscard]] constexpr bool requires_elapsed(time_requirements requirements) noexcept {
        return (static_cast<uint8_t>(requirements) & static_cast<uint8_t>(time_requirements::elapsed)) != 0;
    }

    [[nodiscard]] constexpr time_requirements analyze_time_requirements(std::string_view pattern) noexcept {
        auto requirements = time_requirements::none;

        for (size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] != '%') {
                continue;
            }

            if ((i + 1) >= pattern.size()) {
                break;
            }

            switch (pattern[++i]) {
                case 'H':
                case 'M':
                case 'S':
                case 'e':
                    requirements |= time_requirements::wall_clock;
                    break;
                case '*':
                    requirements |= time_requirements::elapsed;
                    break;
                default:
                    break;
            }
        }

        return requirements;
    }
}  // namespace un::log::backend

namespace un::log {
    using namespace std::literals;

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
            constexpr std::string_view sv() const { return {str.data(), N - 1}; }
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

        template <string_literal Format>
        struct fmt_append_wrapper : fmt_wrapper<Format> {
            consteval fmt_append_wrapper() = default;

            template <typename String, typename... T>
            constexpr auto operator()(String& s, T&&... args) && {
                return fmt::format_to(std::back_inserter(s), Format.sv(), std::forward<T>(args)...);
            }
        };
    }  //  namespace detail

    template <detail::string_literal Pattern>
    struct format_pattern {
        static constexpr bool uses_default_pattern = false;
        static constexpr std::string_view text = Pattern.sv();
        static constexpr backend::time_requirements requirements = backend::analyze_time_requirements(text);
    };

    using default_pattern_plain_type = format_pattern<"[%H:%M:%S.%e] [%*] [%n:%l|%g:%#] >> %v">;
    using default_pattern_color_type = format_pattern<"[%H:%M:%S.%e] [%*] [%n:%^%l%$|%g:%#] >> %v">;

    inline constexpr std::string_view DEFAULT_PATTERN = default_pattern_plain_type::text;
    inline constexpr std::string_view DEFAULT_PATTERN_COLOR = default_pattern_color_type::text;

    struct default_pattern_type {
        static constexpr bool uses_default_pattern = true;
        static constexpr std::string_view text{};
        static constexpr backend::time_requirements requirements = default_pattern_plain_type::requirements;
    };

    namespace literals {
        template <detail::string_literal Format>
        inline consteval auto operator""_format() {
            return detail::fmt_wrapper<Format>{};
        }

        template <detail::string_literal Format>
        inline consteval auto operator""_format_to() {
            return detail::fmt_append_wrapper<Format>{};
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
}  // namespace fmt
