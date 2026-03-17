#pragma once

#include "unlog/channel.hpp"

namespace un::log {
    namespace detail {
        template <typename... Arg>
        inline void log_handle(
                const channel& route,
                log_level level,
                const std::source_location& source_location,
                fmt::format_string<Arg...> fmt,
                Arg&&... args) {
            route.log(sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }

        template <typename... Arg>
        inline void log_default(
                log_level level,
                const std::source_location& source_location,
                fmt::format_string<Arg...> fmt,
                Arg&&... args) {
            global_channel().log(sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }
    }  // namespace detail

    // Objects operating as functions, utilizing CTAD to statically initialize templates for all possible arguments at
    // the point of invocation. This allows for the deduction of the default parameter providing the source location
    // until the point of instantiation
    template <typename... Arg>
    struct trace {
        trace(const channel& route,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::trace, source_location, fmt, std::forward<Arg>(args)...);
        }

        trace(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::trace, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct debug {
        debug(const channel& route,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::debug, source_location, fmt, std::forward<Arg>(args)...);
        }

        debug(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::debug, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct info {
        info(const channel& route,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::info, source_location, fmt, std::forward<Arg>(args)...);
        }

        info(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::info, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct warn {
        warn(const channel& route,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::warn, source_location, fmt, std::forward<Arg>(args)...);
        }

        warn(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::warn, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct critical {
        critical(
                const channel& route,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::critical, source_location, fmt, std::forward<Arg>(args)...);
        }

        critical(
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::critical, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct error {
        error(const channel& route,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, log_level::err, source_location, fmt, std::forward<Arg>(args)...);
        }

        error(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(log_level::err, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct log {
        log(const channel& route,
            log_level level,
            fmt::format_string<Arg...> fmt,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            detail::log_handle(route, level, source_location, fmt, std::forward<Arg>(args)...);
        }

        log(fmt::format_string<Arg...> fmt,
            log_level level,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            detail::log_default(level, source_location, fmt, std::forward<Arg>(args)...);
        }
    };

    // Template deduction guides
    template <typename... Arg>
    trace(const channel&, fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;
    template <typename... Arg>
    trace(fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;

    template <typename... Arg>
    debug(const channel&, fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;
    template <typename... Arg>
    debug(fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;

    template <typename... Arg>
    info(const channel&, fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;
    template <typename... Arg>
    info(fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;

    template <typename... Arg>
    warn(const channel&, fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;
    template <typename... Arg>
    warn(fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;

    template <typename... Arg>
    error(const channel&, fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;
    template <typename... Arg>
    error(fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;

    template <typename... Arg>
    critical(const channel&, fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;
    template <typename... Arg>
    critical(fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;

    template <typename... Arg>
    log(const channel&, log_level, fmt::format_string<Arg...>, Arg&&...) -> log<Arg...>;
    template <typename... Arg>
    log(fmt::format_string<Arg...>, log_level, Arg&&...) -> log<Arg...>;

    // Exposed API functions
    template <detail::basic_config_type Conf>
    inline channel make_channel(const Conf& conf, bool make_default = false) {
        return detail::make_channel(conf, make_default);
    }

    inline channel make_channel(std::string_view name, bool make_default = false) {
        return detail::make_channel(config<>::make(name), make_default);
    }

    void set_global_config(global_config cfg);

    global_config get_global_config();

    void prewarm_thread();

    inline void set_default_level(log_level level = log_level::info) {
        return detail::set_default_level(level);
    }

    inline log_level get_default_level() {
        return detail::get_default_level();
    }

    void flush();
}  // namespace un::log

// Global namespace alias
namespace unlog = un::log;
