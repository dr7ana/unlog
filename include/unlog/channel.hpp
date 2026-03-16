#pragma once

#include "config.hpp"
#include "format.hpp"

#include "unlog/backend/backend.hpp"
#include "unlog/backend/producer.hpp"
#include "unlog/backend/record.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace un::log {

    using sink_ptr = backend::sink_ptr;

    class channel;

    namespace detail {
        using clock_now_fn_t = uint64_t (*)() noexcept;

        inline constexpr bool level_enabled(log_level message_level, log_level threshold) {
            if (threshold == log_level::off)
                return false;
            if (message_level == log_level::off)
                return false;

            return static_cast<uint8_t>(message_level) >= static_cast<uint8_t>(threshold);
        }

        backend::runtime_queue_producer& get_runtime_queue_producer(RuntimeMode runtime_mode);
        void note_runtime_work_available() noexcept;
        void mark_runtime_active_after_commit(uint64_t sequence) noexcept;

        template <typename... Arg>
        void log_message(
                channel_id channel,
                RuntimeMode runtime_mode,
                clock_now_fn_t clock_now_fn,
                const char* channel_name,
                size_t max_message_size,
                bool overflow_drop,
                bool can_truncate,
                const source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) {
            if (!clock_now_fn || !channel_name)
                return;

            auto& producer = get_runtime_queue_producer(runtime_mode);
            auto slot = backend::runtime_record_slot{};
            auto timestamp = clock_now_fn();
            auto sequence = producer.next_sequence();
            auto payload_limit = can_truncate ? max_message_size : size_t{0};
            auto result = backend::write_record_slot(
                    slot,
                    channel,
                    level,
                    timestamp,
                    producer.thread_id(),
                    sequence,
                    source_location,
                    overflow_drop ? OverflowPolicy::drop : OverflowPolicy::truncate,
                    payload_limit,
                    format,
                    std::forward<Arg>(args)...);

            if (result == backend::record_slot_write_result::dropped) {
                producer.count_dropped();
                return;
            }

            if (!producer.queue().emplace(std::move(slot))) {
                producer.count_dropped();
                return;
            }

            producer.count_emitted();
            if (result == backend::record_slot_write_result::truncated)
                producer.count_truncated();

            note_runtime_work_available();
            mark_runtime_active_after_commit(sequence);
        }

        struct channel_runtime_view {
            RuntimeMode runtime_mode{RuntimeMode::single_threaded};
            clock_now_fn_t clock_now_fn{nullptr};
            const char* channel_name{nullptr};
            size_t max_message_size{0};
            bool overflow_drop{true};
            bool can_truncate{false};
            log_level threshold{log_level::off};
        };

        void set_channel_level(channel_id id, log_level level) noexcept;
        log_level channel_level(channel_id id) noexcept;
        channel_runtime_view channel_runtime_view_for(channel_id id) noexcept;

        inline size_t resolve_channel_max_message_size(size_t max_record_size) {
            auto max_message_size = backend::max_message_size_for_runtime_record_limit(max_record_size);
            if (max_message_size.has_value())
                return *max_message_size;

            switch (backend::runtime_record_limit_status(max_record_size)) {
                case backend::runtime_record_limit_result::too_small:
                    throw std::invalid_argument{"channel max_record_size is too small to fit record metadata"};
                case backend::runtime_record_limit_result::exceeds_runtime_slot:
                    throw std::invalid_argument{"channel max_record_size exceeds runtime queue slot payload capacity"};
                case backend::runtime_record_limit_result::supported:
                    break;
            }

            throw std::invalid_argument{"channel max_record_size is invalid"};
        }

        struct channel_registration {
            std::string_view name;
            size_t max_message_size{0};
            bool overflow_drop{true};
            bool can_truncate{false};
            ClockType timestamp_mode{ClockType::steady};
            SinkType sink_type{SinkType::cout};
            std::string_view format;
            std::optional<fs::path> filename{};
            std::optional<int> output_fd{};
            std::optional<fs::path> unix_dgram_path{};
        };

        channel make_channel_route(channel_registration registration, bool make_default);
    }  // namespace detail

    class channel {
      public:
        constexpr channel() noexcept = default;
        constexpr channel(channel_id id, std::string_view name) noexcept : id_{id}, name_{name} {}

        constexpr explicit operator bool() const noexcept { return id_ != invalid_channel_id; }

        template <typename... Arg>
        void log(
                const detail::source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) const {
            if (!*this)
                return;

            auto view = detail::channel_runtime_view_for(id_);

            if (!detail::level_enabled(level, view.threshold))
                return;

            if (!view.clock_now_fn)
                return;

            detail::log_message(
                    id_,
                    view.runtime_mode,
                    view.clock_now_fn,
                    view.channel_name,
                    view.max_message_size,
                    view.overflow_drop,
                    view.can_truncate,
                    source_location,
                    level,
                    format,
                    std::forward<Arg>(args)...);
        }

        void set_level(log_level level) const noexcept {
            if (*this) {
                detail::set_channel_level(id_, level);
            }
        }

        [[nodiscard]] log_level level() const noexcept {
            if (!*this) {
                return log_level::off;
            }

            return detail::channel_level(id_);
        }

        [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
        [[nodiscard]] constexpr channel_id id() const noexcept { return id_; }

      private:
        channel_id id_{invalid_channel_id};
        std::string_view name_{};
    };

    namespace detail {

        template <basic_config_type Conf>
        channel make_channel(const Conf& conf, bool make_default) {
            auto max_message_size = resolve_channel_max_message_size(static_cast<size_t>(Conf::max_record_size));

            return make_channel_route(
                    channel_registration{
                            .name = conf.name,
                            .max_message_size = max_message_size,
                            .overflow_drop = config_overflow_policy_v<Conf> == OverflowPolicy::drop,
                            .can_truncate = true,
                            .timestamp_mode = config_clock_type_v<Conf>,
                            .sink_type = config_sink_type(conf),
                            .format = conf.format,
                            .filename = config_filename(conf),
                            .output_fd = config_output_fd(conf),
                            .unix_dgram_path = config_unix_dgram_path(conf),
                    },
                    make_default);
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

    channel global_channel();

}  // namespace un::log
