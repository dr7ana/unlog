#pragma once

#include "config.hpp"
#include "format.hpp"

#include "unlog/backend/backend.hpp"
#include "unlog/backend/producer.hpp"
#include "unlog/backend/record.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace un::log {

    using sink_ptr = backend::sink_ptr;

    namespace detail {
        template <
                RuntimeMode Mode,
                ClockType Clock,
                OverflowPolicy Overflow,
                bool HugePages,
                backend::time_requirements TimeRequirements>
        struct channel_policy {
            static constexpr auto runtime_mode = Mode;
            static constexpr auto clock_type = Clock;
            static constexpr auto overflow_policy = Overflow;
            static constexpr bool huge_pages = HugePages;
            static constexpr auto time_requirements = TimeRequirements;
        };

    }  // namespace detail

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept channel_policy_type = requires {
        { U::runtime_mode } -> std::convertible_to<RuntimeMode>;
        { U::clock_type } -> std::convertible_to<ClockType>;
        { U::overflow_policy } -> std::convertible_to<OverflowPolicy>;
        { U::huge_pages } -> std::convertible_to<bool>;
        { U::time_requirements } -> std::convertible_to<backend::time_requirements>;
    };

    template <channel_policy_type Policy>
    class channel;

    namespace detail {

        template <basic_config_type Conf>
        using channel_policy_for = channel_policy<
                config_runtime_mode_v<Conf>,
                config_clock_type_v<Conf>,
                config_overflow_policy_v<Conf>,
                Conf::use_huge_pages,
                Conf::format_requirements>;

        using default_channel_policy = channel_policy_for<config<>>;

        template <basic_config_type Conf>
        constexpr channel<channel_policy_for<Conf>> make_channel(const Conf& conf);

        struct channel_handle {
            channel_id id{invalid_channel_id};
            std::string_view name{};
            size_t max_message_size{0};
        };

        template <typename Clock>
        concept log_clock = requires {
            typename Clock::time_point;
            { Clock::now() } noexcept -> std::same_as<typename Clock::time_point>;
            { Clock::is_steady } -> std::convertible_to<const bool&>;
        };

        template <log_clock Clock>
        inline constexpr uint64_t clock_now_ns() noexcept {
            auto now = Clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        }

        template <ClockType Type>
        inline constexpr uint64_t clock_now_ns_for() noexcept {
            if constexpr (Type == ClockType::system) {
                return clock_now_ns<std::chrono::system_clock>();
            }

            return clock_now_ns<std::chrono::steady_clock>();
        }

        inline constexpr bool level_enabled(log_level message_level, log_level threshold) {
            if (threshold == log_level::off) {
                return false;
            }
            if (message_level == log_level::off) {
                return false;
            }

            return static_cast<uint8_t>(message_level) >= static_cast<uint8_t>(threshold);
        }

        template <bool HugePages>
        using runtime_queue_producer_t = backend::runtime_queue_producer<HugePages>;

        runtime_queue_producer_t<false>& single_threaded_runtime_queue_producer_normal();
        runtime_queue_producer_t<true>& single_threaded_runtime_queue_producer_huge();
        runtime_queue_producer_t<false>& ensure_threadsafe_runtime_queue_producer_normal();
        runtime_queue_producer_t<true>& ensure_threadsafe_runtime_queue_producer_huge();

        template <un::log::channel_policy_type Policy>
        runtime_queue_producer_t<Policy::huge_pages>& get_runtime_queue_producer() {
            if constexpr (Policy::runtime_mode == RuntimeMode::single_threaded) {
                if constexpr (Policy::huge_pages) {
                    return single_threaded_runtime_queue_producer_huge();
                }
                else {
                    return single_threaded_runtime_queue_producer_normal();
                }
            }
            else {
                if constexpr (Policy::huge_pages) {
                    return ensure_threadsafe_runtime_queue_producer_huge();
                }
                else {
                    return ensure_threadsafe_runtime_queue_producer_normal();
                }
            }
        }

        void notify_runtime_work_available() noexcept;

        template <un::log::channel_policy_type Policy>
        void note_runtime_work_available(runtime_queue_producer_t<Policy::huge_pages>& producer) noexcept {
            if (!producer.try_mark_enqueued()) {
                return;
            }

            if constexpr (Policy::runtime_mode == RuntimeMode::threadsafe) {
                producer.publish_ready_bit();
            }

            notify_runtime_work_available();
        }

        void mark_runtime_active_after_commit() noexcept;

        struct produce_record_outcome {
            bool publish{false};
            bool truncated{false};
            std::exception_ptr error{};
        };

        template <typename... Arg>
        [[nodiscard]] constexpr produce_record_outcome produce_runtime_record(
                backend::runtime_record_slot* slot,
                auto& producer,
                channel_id channel,
                log_level level,
                uint64_t timestamp,
                uint64_t sequence,
                size_t payload_limit,
                bool overflow_drop,
                const source_loc& source_location,
                fmt::format_string<Arg...> format,
                Arg&&... args) noexcept {
            auto outcome = produce_record_outcome{};
            auto constructed = false;

            try {
                std::construct_at(slot);
                constructed = true;

                auto result = backend::write_record_slot(
                        *slot,
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

                if (result == backend::record_slot_write_result::written) {
                    outcome.publish = true;
                    return outcome;
                }

                if (result == backend::record_slot_write_result::truncated) {
                    outcome.publish = true;
                    outcome.truncated = true;
                    return outcome;
                }

                std::destroy_at(slot);
                return outcome;
            } catch (...) {
                if (constructed) {
                    std::destroy_at(slot);
                }

                outcome.error = std::current_exception();
                return outcome;
            }
        }

        template <un::log::channel_policy_type Policy, typename... Arg>
        constexpr void log_message(
                channel_id channel,
                const char* channel_name,
                size_t max_message_size,
                const source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) {
            if (!channel_name) {
                return;
            }

            auto& producer = get_runtime_queue_producer<Policy>();
            auto timestamp = clock_now_ns_for<Policy::clock_type>();
            auto sequence = producer.next_sequence();
            auto outcome = produce_record_outcome{};
            auto published = producer.queue().produce([&](backend::runtime_record_slot* slot) noexcept -> bool {
                outcome = produce_runtime_record(
                        slot,
                        producer,
                        channel,
                        level,
                        timestamp,
                        sequence,
                        max_message_size,
                        Policy::overflow_policy == OverflowPolicy::drop,
                        source_location,
                        format,
                        std::forward<Arg>(args)...);
                return outcome.publish;
            });

            if (outcome.error) {
                std::rethrow_exception(outcome.error);
            }

            if (!published) {
#if UNLOG_DIAGNOSTIC
                producer.count_dropped();
#endif
                return;
            }

#if UNLOG_DIAGNOSTIC
            producer.count_emitted();
            if (outcome.truncated) {
                producer.count_truncated();
            }
#endif

            note_runtime_work_available<Policy>(producer);
            mark_runtime_active_after_commit();
        }

        void set_channel_level(channel_id id, log_level level) noexcept;
        log_level channel_level(channel_id id) noexcept;

        inline constexpr size_t resolve_channel_max_message_size(size_t max_record_size) {
            auto max_message_size = backend::max_message_size_for_runtime_record_limit(max_record_size);
            if (max_message_size.has_value()) {
                return *max_message_size;
            }

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
            RuntimeMode runtime_mode{RuntimeMode::single_threaded};
            ClockType timestamp_mode{ClockType::steady};
            bool huge_pages{false};
            SinkType sink_type{SinkType::cout};
            std::string_view format;
            bool color{false};
            backend::time_requirements time_requirements{backend::time_requirements::none};
            std::optional<fs::path> filename{};
            std::optional<int> output_fd{};
            std::optional<fs::path> unix_dgram_path{};
        };

        channel_handle make_channel_route(channel_registration registration);
    }  // namespace detail

    template <channel_policy_type Policy>
    class channel {
      public:
        using policy_type = Policy;

        channel() = delete;

        constexpr explicit operator bool() const noexcept { return id_ != invalid_channel_id; }

        template <typename... Arg>
        constexpr void log(
                const detail::source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) const {
            if (!*this) {
                return;
            }

            if (!detail::level_enabled(level, detail::channel_level(id_))) {
                return;
            }

            detail::log_message<Policy>(
                    id_, name_.data(), max_message_size_, source_location, level, format, std::forward<Arg>(args)...);
        }

        constexpr void set_level(log_level level) const noexcept {
            if (*this) {
                detail::set_channel_level(id_, level);
            }
        }

        [[nodiscard]] constexpr log_level level() const noexcept {
            if (!*this) {
                return log_level::off;
            }

            return detail::channel_level(id_);
        }

        [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
        [[nodiscard]] constexpr channel_id id() const noexcept { return id_; }

      private:
        template <detail::basic_config_type Conf>
        constexpr friend channel<detail::channel_policy_for<Conf>> detail::make_channel(const Conf& conf);

        constexpr channel(channel_id id, std::string_view name, size_t max_message_size) noexcept :
                id_{id}, name_{name}, max_message_size_{max_message_size} {}

        channel_id id_{invalid_channel_id};
        std::string_view name_{};
        size_t max_message_size_{0};
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept channel_type = requires { typename U::policy_type; } && channel_policy_type<typename U::policy_type> &&
                           std::same_as<U, channel<typename U::policy_type>>;

    using default_channel = channel<detail::default_channel_policy>;

    static_assert(channel_policy_type<detail::default_channel_policy>);
    static_assert(channel_type<default_channel>);

    namespace detail {

        template <basic_config_type Conf>
        constexpr channel<channel_policy_for<Conf>> make_channel(const Conf& conf) {
            auto max_message_size = resolve_channel_max_message_size(static_cast<size_t>(Conf::max_record_size));
            auto handle = make_channel_route(
                    channel_registration{
                            .name = conf.name,
                            .max_message_size = max_message_size,
                            .overflow_drop = config_overflow_policy_v<Conf> == OverflowPolicy::drop,
                            .can_truncate = true,
                            .runtime_mode = config_runtime_mode_v<Conf>,
                            .timestamp_mode = config_clock_type_v<Conf>,
                            .huge_pages = Conf::use_huge_pages,
                            .sink_type = config_sink_type(conf),
                            .format = conf.format,
                            .color = conf.color(),
                            .time_requirements = config_format_requirements(conf),
                            .filename = config_filename(conf),
                            .output_fd = config_output_fd(conf),
                            .unix_dgram_path = config_unix_dgram_path(conf),
                    });

            return channel<channel_policy_for<Conf>>{handle.id, handle.name, max_message_size};
        }

        void add_sink_route(ClockType timestamp_mode, backend::sink_entry entry);

        template <basic_config_type Conf>
        inline void add_sink(const Conf& conf, sink_ptr sink) {
            add_sink_route(
                    config_clock_type_v<Conf>,
                    backend::sink_entry{
                            .sink = std::move(sink),
                            .pattern = std::string{conf.format},
                            .color = conf.color(),
                            .requirements = config_format_requirements(conf),
                    });
        }

        void flush_backend();

#if UNLOG_DIAGNOSTIC
        backend::producer_stats backend_stats();
#endif

        log_level get_current_level();

        void set_current_level(log_level level);

    }  // namespace detail

}  // namespace un::log
