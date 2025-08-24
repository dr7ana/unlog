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
                logger->log(detail::sloc(source_location), LogLevel::trace, fmt, std::forward<Arg>(args)...);
        }

        trace(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::trace, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct debug {
        debug(const logger_ptr& logger,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), LogLevel::debug, fmt, std::forward<Arg>(args)...);
        }

        debug(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::debug, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct info {
        info(const logger_ptr& logger,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), LogLevel::info, fmt, std::forward<Arg>(args)...);
        }

        info(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::info, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct warn {
        warn(const logger_ptr& logger,
             fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), LogLevel::warn, fmt, std::forward<Arg>(args)...);
        }

        warn(fmt::format_string<Arg...> fmt,
             Arg&&... args,
             const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::warn, fmt, std::forward<Arg>(args)...);
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
                logger->log(detail::sloc(source_location), LogLevel::critical, fmt, std::forward<Arg>(args)...);
        }

        critical(
                fmt::format_string<Arg...> fmt,
                Arg&&... args,
                const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::critical, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct error {
        error(const logger_ptr& logger,
              fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), LogLevel::err, fmt, std::forward<Arg>(args)...);
        }

        error(fmt::format_string<Arg...> fmt,
              Arg&&... args,
              const std::source_location& source_location = std::source_location::current()) {
            global_logger()->log(detail::sloc(source_location), LogLevel::err, fmt, std::forward<Arg>(args)...);
        }
    };

    template <typename... Arg>
    struct log {
        log(const logger_ptr& logger,
            LogLevel level,
            fmt::format_string<Arg...> fmt,
            Arg&&... args,
            const std::source_location& source_location = std::source_location::current()) {
            if (logger)
                logger->log(detail::sloc(source_location), level, fmt, std::forward<Arg>(args)...);
        }

        log(fmt::format_string<Arg...> fmt,
            LogLevel level,
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
    log(const logger_ptr&, LogLevel, fmt::format_string<Arg...>, Arg&&...) -> log<Arg...>;
    template <typename... Arg>
    log(fmt::format_string<Arg...>, LogLevel, Arg&&...) -> log<Arg...>;

    inline void reset_level(LogLevel level = LogLevel::info) {
        return detail::reset_level(level);
    }

    inline void set_default_level(LogLevel level) {
        return detail::set_default_level(level);
    }

    inline LogLevel get_default_level() {
        return detail::get_default_level();
    }

    template <spdlog_sink_t T, typename... Arg>
    inline void add_sink(Arg... args) {
        return detail::add_sink(std::make_shared<T>(std::forward<Arg>(args)...));
    }

    void flush();
}  // namespace un::log

// Global namespace alias
namespace unlog = un::log;
