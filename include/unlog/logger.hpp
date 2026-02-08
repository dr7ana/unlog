#pragma once

#include "config.hpp"
#include "format.hpp"

#include "unlog/backend/backend.hpp"
#include "unlog/backend/record.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace un::log {

    using sink_ptr = backend::sink_ptr;

    namespace detail {
        using clock_now_fn_t = uint64_t (*)() noexcept;

        inline constexpr bool level_enabled(log_level message_level, log_level threshold) {
            if (threshold == log_level::off)
                return false;
            if (message_level == log_level::off)
                return false;

            return static_cast<uint8_t>(message_level) >= static_cast<uint8_t>(threshold);
        }

        void mark_runtime_active_after_commit(uint64_t sequence) noexcept;

        void log_message(
                backend::sqpoll_backend* active_backend,
                clock_now_fn_t clock_now_fn,
                const char* logger_name,
                size_t max_message_size,
                bool overflow_drop,
                bool can_truncate,
                const source_loc& source_location,
                log_level level,
                std::string&& rendered);
    }  // namespace detail

    class logger {
      public:
        template <detail::basic_config_type Conf>
        explicit logger(Conf conf) :
                name_{std::move(conf.name)},
                max_message_size_{
                        backend::max_message_size_for_record_limit(static_cast<size_t>(Conf::max_record_size))
                                .value_or(size_t{0}),
                },
                overflow_drop_{detail::config_overflow_policy_v<Conf> == OverflowPolicy::drop},
                can_truncate_{
                        backend::max_message_size_for_record_limit(static_cast<size_t>(Conf::max_record_size))
                                .has_value(),
                },
                level_{log_level::info} {}

        template <typename... Arg>
        void log(
                const detail::source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) {
            if (!detail::level_enabled(level, level_.load(std::memory_order_relaxed)))
                return;

            if (!active_backend_ || !clock_now_fn_)
                return;

            detail::log_message(
                    active_backend_,
                    clock_now_fn_,
                    name_.c_str(),
                    max_message_size_,
                    overflow_drop_,
                    can_truncate_,
                    source_location,
                    level,
                    fmt::format(format, std::forward<Arg>(args)...));
        }

        void set_level(log_level level);
        log_level level() const;

        void bind_backend(backend::sqpoll_backend* active_backend, detail::clock_now_fn_t clock_now_fn) noexcept {
            active_backend_ = active_backend;
            clock_now_fn_ = clock_now_fn;
        }

        constexpr std::string_view name() const noexcept { return name_; }
        constexpr backend::sqpoll_backend* active_backend() const noexcept { return active_backend_; }

      private:
        std::string name_;
        size_t max_message_size_{0};
        bool overflow_drop_{true};
        bool can_truncate_{false};
        backend::sqpoll_backend* active_backend_{nullptr};
        detail::clock_now_fn_t clock_now_fn_{nullptr};
        std::atomic<log_level> level_{log_level::info};
    };

    using logger_ptr = std::shared_ptr<logger>;

    namespace detail {

        void make_logger_route(
                logger_ptr created,
                bool make_default,
                std::string_view logger_name,
                ClockType timestamp_mode,
                SinkType sink_type,
                std::string_view format,
                std::optional<fs::path> filename,
                bool strict_nonblocking,
                size_t sqpoll_queue_depth,
                std::optional<int> output_fd,
                std::optional<fs::path> unix_dgram_path);

        template <basic_config_type Conf>
        void make_logger(const Conf& conf, bool make_default) {
            auto created = std::make_shared<logger>(conf);
            make_logger_route(
                    std::move(created),
                    make_default,
                    conf.name,
                    config_clock_type_v<Conf>,
                    config_sink_type(conf),
                    conf.format,
                    config_filename(conf),
                    config_strict_nonblocking(conf),
                    config_sqpoll_queue_depth(conf),
                    config_output_fd(conf),
                    config_unix_dgram_path(conf));
        }

        void add_sink_route(sink_ptr sink, ClockType timestamp_mode, std::string_view format);

        template <basic_config_type Conf>
        inline void add_sink(const Conf& conf, sink_ptr sink) {
            add_sink_route(std::move(sink), config_clock_type_v<Conf>, conf.format);
        }

        void flush_backend();

        backend::producer_stats backend_stats();

        log_level get_default_level();

        void set_default_level(log_level level);

    }  // namespace detail

    const logger_ptr& global_logger();

}  // namespace un::log
