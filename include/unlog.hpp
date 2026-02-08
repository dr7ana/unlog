#pragma once

#include "unlog/logger.hpp"

namespace un::log {
    // Objects operating as functions, utilizing CTAD to statically initialize templates for all possible arguments at
    // the point of invocation. This allows for the deduction of the default parameter providing the source location
    // until the point of instantiation
    template <typename... Arg>
    struct trace {
        trace(const logger_ptr& logger,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::trace, fmt, std::forward<Arg>(args)...);
        }

        trace(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::trace, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct debug {
        debug(const logger_ptr& logger,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::debug, fmt, std::forward<Arg>(args)...);
        }

        debug(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::debug, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct info {
        info(const logger_ptr& logger,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::info, fmt, std::forward<Arg>(args)...);
        }

        info(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::info, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct warn {
        warn(const logger_ptr& logger,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::warn, fmt, std::forward<Arg>(args)...);
        }

        warn(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::warn, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct critical {
        critical(
                const logger_ptr& logger,
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::critical, fmt, std::forward<Arg>(args)...);
        }

        critical(
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::critical, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct error {
        error(const logger_ptr& logger,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), log_level::err, fmt, std::forward<Arg>(args)...);
        }

        error(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), log_level::err, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct log {
        log(const logger_ptr& logger,
            log_level level,
            fmt::format_string<Arg...> fmt,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }

        log(fmt::format_string<Arg...> fmt,
            log_level level,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }
    };

    // Template deduction guides
    template <typename... Arg>
    trace(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;
    template <typename... Arg>
    trace(fmt::format_string<Arg...>, Arg&&...) -> trace<Arg...>;

    template <typename... Arg>
    debug(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;
    template <typename... Arg>
    debug(fmt::format_string<Arg...>, Arg&&...) -> debug<Arg...>;

    template <typename... Arg>
    info(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;
    template <typename... Arg>
    info(fmt::format_string<Arg...>, Arg&&...) -> info<Arg...>;

    template <typename... Arg>
    warn(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;
    template <typename... Arg>
    warn(fmt::format_string<Arg...>, Arg&&...) -> warn<Arg...>;

    template <typename... Arg>
    error(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;
    template <typename... Arg>
    error(fmt::format_string<Arg...>, Arg&&...) -> error<Arg...>;

    template <typename... Arg>
    critical(const logger_ptr&, fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;
    template <typename... Arg>
    critical(fmt::format_string<Arg...>, Arg&&...) -> critical<Arg...>;

    template <typename... Arg>
    log(const logger_ptr&, log_level, fmt::format_string<Arg...>, Arg&&...) -> log<Arg...>;
    template <typename... Arg>
    log(fmt::format_string<Arg...>, log_level, Arg&&...) -> log<Arg...>;

    // Exposed API functions
    template <detail::basic_config_type Conf>
    inline void make_logger(const Conf& conf, bool make_default = false) {
        return detail::make_logger(conf, make_default);
    }

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
