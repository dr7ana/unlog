#pragma once

#include "unlog/channel.hpp"

namespace un::log {
    namespace detail {
        template <global_config Global, typename Policy, typename... Arg>
        constexpr void log_handle(
                const channel<Global, Policy>& route,
                log_level level,
                const std::source_location& source_location,
                fmt::format_string<Arg...> fmt,
                Arg&&... args) {
            if (!route.enabled(level)) {
                return;
            }

            route.log(sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }

    }  // namespace detail

    // Objects operating as functions, utilizing CTAD to statically initialize templates for all possible arguments at
    // the point of invocation. This allows for the deduction of the default parameter providing the source location
    // until the point of instantiation
    template <typename... Arg>
    struct trace {
        template <global_config Global, typename Policy>
        constexpr trace(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::trace, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct debug {
        template <global_config Global, typename Policy>
        constexpr debug(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::debug, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct info {
        template <global_config Global, typename Policy>
        constexpr info(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::info, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct warn {
        template <global_config Global, typename Policy>
        constexpr warn(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::warn, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct critical {
        template <global_config Global, typename Policy>
        constexpr critical(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::critical, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct error {
        template <global_config Global, typename Policy>
        constexpr error(
                const channel<Global, Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::err, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct log {
        template <global_config Global, typename Policy>
        constexpr log(
                const channel<Global, Policy>& route,
                log_level level,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, level, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    // Template deduction guides
    template <global_config Global, typename Policy, typename... Arg>
    trace(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;
    template <global_config Global, typename Policy, typename... Arg>
    debug(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;

    template <global_config Global, typename Policy, typename... Arg>
    info(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;

    template <global_config Global, typename Policy, typename... Arg>
    warn(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;

    template <global_config Global, typename Policy, typename... Arg>
    error(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;

    template <global_config Global, typename Policy, typename... Arg>
    critical(const channel<Global, Policy>&, fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;

    template <global_config Global, typename Policy, typename... Arg>
    log(const channel<Global, Policy>&, log_level, fmt::format_string<Arg...>, Arg&&...) -> log<Arg...>;
}  // namespace un::log

// Global namespace alias
namespace unlog = un::log;
