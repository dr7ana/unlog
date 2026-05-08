#pragma once

#include "unlog/channel.hpp"

namespace un::log {
    namespace detail {
        template <typename Policy, typename... Arg>
        constexpr void log_handle(
                const channel<Policy>& route,
                log_level level,
                const std::source_location& source_location,
                fmt::format_string<Arg...> fmt,
                Arg&&... args) {
            route.log(sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }

    }  // namespace detail

    // Objects operating as functions, utilizing CTAD to statically initialize templates for all possible arguments at
    // the point of invocation. This allows for the deduction of the default parameter providing the source location
    // until the point of instantiation
    template <typename... Arg>
    struct trace {
        template <typename Policy>
        constexpr trace(
                const channel<Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::trace, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct debug {
        template <typename Policy>
        constexpr debug(
                const channel<Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::debug, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct info {
        template <typename Policy>
        constexpr info(
                const channel<Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::info, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct warn {
        template <typename Policy>
        warn(const channel<Policy>& route,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::warn, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct critical {
        template <typename Policy>
        critical(
                const channel<Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::critical, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct error {
        template <typename Policy>
        constexpr error(
                const channel<Policy>& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::err, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct log {
        template <typename Policy>
        log(const channel<Policy>& route,
            log_level level,
            fmt::format_string<Arg...> fmt,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, level, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    // Template deduction guides
    template <typename Policy, typename... Arg>
    trace(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;
    template <typename Policy, typename... Arg>
    debug(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;

    template <typename Policy, typename... Arg>
    info(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;

    template <typename Policy, typename... Arg>
    warn(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;

    template <typename Policy, typename... Arg>
    error(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;

    template <typename Policy, typename... Arg>
    critical(const channel<Policy>&, fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;

    template <typename Policy, typename... Arg>
    log(const channel<Policy>&, log_level, fmt::format_string<Arg...>, Arg&&...) -> log<Arg...>;

    // Exposed API functions
    template <detail::basic_config_type Conf>
    inline constexpr auto make_channel(const Conf& conf) {
        return detail::make_channel(conf);
    }

    void set_global_config(global_config cfg);

    global_config get_global_config();

    void prewarm_thread();

    inline void set_current_level(log_level level = log_level::info) {
        return detail::set_current_level(level);
    }

    inline log_level get_current_level() {
        return detail::get_current_level();
    }

    void flush();
}  // namespace un::log

// Global namespace alias
namespace unlog = un::log;
