#pragma once

#include "config.hpp"
#include "format.hpp"

#include "unlog/backend/backend.hpp"
#include "unlog/backend/producer.hpp"
#include "unlog/backend/record.hpp"

#include <atomic>
#include <bit>
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
        struct route_state {
            route_state(channel_id route_id, std::string route_name) : id{route_id}, name{std::move(route_name)} {}

            void configure_level(log_level value) noexcept { configured_level_ = value; }
            void set_running_level(log_level value) noexcept {
                configured_level_ = value;
                level_.store(value, std::memory_order_relaxed);
            }
            void activate() noexcept { level_.store(configured_level_, std::memory_order_relaxed); }
            void deactivate() noexcept { level_.store(log_level::off, std::memory_order_relaxed); }
            [[nodiscard]] log_level level() const noexcept { return level_.load(std::memory_order_relaxed); }

            channel_id id{invalid_channel_id};
            std::string name;
            backend::sink_entry sink;

          private:
            log_level configured_level_{log_level::info};
            std::atomic<log_level> level_{log_level::off};
        };

        template <
                RuntimeMode Mode,
                OverflowPolicy Overflow,
                bool HugePages,
                backend::time_requirements TimeRequirements,
                bool CaptureSourceFile,
                bool CaptureSourceLine>
        struct channel_policy {
            static constexpr auto runtime_mode = Mode;
            static constexpr auto overflow_policy = Overflow;
            static constexpr bool huge_pages = HugePages;
            static constexpr auto time_requirements = TimeRequirements;
            static constexpr bool capture_source_file = CaptureSourceFile;
            static constexpr bool capture_source_line = CaptureSourceLine;
        };

    }  // namespace detail

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept channel_policy_type = requires {
        { U::runtime_mode } -> std::convertible_to<RuntimeMode>;
        { U::overflow_policy } -> std::convertible_to<OverflowPolicy>;
        { U::huge_pages } -> std::convertible_to<bool>;
        { U::time_requirements } -> std::convertible_to<backend::time_requirements>;
        { U::capture_source_file } -> std::convertible_to<bool>;
        { U::capture_source_line } -> std::convertible_to<bool>;
    };

    template <global_config Global, channel_policy_type Policy>
    class channel;

    template <global_config Global = default_global_config>
    struct configured;

    namespace detail {

        template <basic_config_type Conf>
        using channel_policy_for = channel_policy<
                config_runtime_mode_v<Conf>,
                config_overflow_policy_v<Conf>,
                Conf::use_huge_pages,
                Conf::format_requirements,
                Conf::capture_source_file,
                Conf::capture_source_line>;

        using default_channel_policy = channel_policy_for<config<>>;

        template <global_config Global, basic_config_type Conf>
        constexpr channel<Global, channel_policy_for<Conf>> make_channel(const Conf& conf);

        struct channel_handle {
            route_state* route{nullptr};
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

        struct produce_record_outcome {
            bool publish{false};
            bool truncated{false};
            std::exception_ptr error{};
        };

        template <global_config Global, channel_policy_type Policy, typename Slot, typename... Arg>
        [[nodiscard]] constexpr produce_record_outcome produce_runtime_record(
                Slot* slot,
                const route_state* route,
                log_level level,
                bool overflow_drop,
                const source_loc& source_location,
                fmt::format_string<Arg...> format,
                Arg&&... args) noexcept {
            auto outcome = produce_record_outcome{};
            auto constructed = false;

            try {
                std::construct_at(
                        slot,
                        route,
                        level,
                        [] {
                            if constexpr (Policy::time_requirements != backend::time_requirements::none) {
                                return clock_now_ns_for<Global.clock_type>();
                            }
                            return uint64_t{0};
                        }(),
                        Policy::capture_source_file ? source_location.filename : nullptr,
                        Policy::capture_source_line ? static_cast<int32_t>(source_location.line) : int32_t{0});
                constructed = true;

                auto result = backend::write_record_slot(
                        *slot,
                        overflow_drop ? OverflowPolicy::drop : OverflowPolicy::truncate,
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

        template <global_config Global, un::log::channel_policy_type Policy, typename... Arg>
        constexpr void log_message(
                backend::configured_producer_backend<Global>& runtime,
                const route_state* route,
                const source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) {
            auto& producer = runtime.template producer<Policy>();
            auto outcome = produce_record_outcome{};
            auto published = producer.queue().produce([&](auto* slot) noexcept -> bool {
                outcome = produce_runtime_record<Global, Policy>(
                        slot,
                        route,
                        level,
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

            runtime.template note_work<Policy>(producer);
        }

        struct channel_registration {
            std::string_view name;
            RuntimeMode runtime_mode{RuntimeMode::single_threaded};
            bool huge_pages{false};
            SinkType sink_type{SinkType::cout};
            backend::pattern_program pattern;
            bool color{false};
            backend::time_requirements time_requirements{backend::time_requirements::none};
            std::optional<fs::path> filename{};
            std::optional<int> output_fd{};
            std::optional<fs::path> unix_dgram_path{};
        };

        using producer_backend_factory = backend::runtime_producer_backend* (*)();
        backend::runtime_producer_backend& access_producer_backend(global_config config, producer_backend_factory make);
        channel_handle make_channel_route(
                backend::runtime_producer_backend& producer_backend, channel_registration registration);
        void set_route_level(
                backend::runtime_producer_backend& producer_backend, route_state& route, log_level level) noexcept;
    }  // namespace detail

    template <global_config Global, channel_policy_type Policy>
    class channel {
      public:
        static constexpr auto global = Global;
        using policy_type = Policy;

        channel() = delete;

        constexpr explicit operator bool() const noexcept { return route_ != nullptr; }

        [[nodiscard]] constexpr bool enabled(log_level level) const noexcept {
            return *this && detail::level_enabled(level, route_->level());
        }

        template <typename... Arg>
        constexpr void log(
                const detail::source_loc& source_location,
                log_level level,
                fmt::format_string<Arg...> format,
                Arg&&... args) const {
            if (!*this) {
                return;
            }

            detail::log_message<Global, Policy>(
                    *producer_backend_, route_, source_location, level, format, std::forward<Arg>(args)...);
        }

        void set_level(log_level level) const noexcept {
            if (*this) {
                detail::set_route_level(*producer_backend_, *route_, level);
            }
        }

        [[nodiscard]] constexpr log_level level() const noexcept {
            if (!*this) {
                return log_level::off;
            }

            return route_->level();
        }

        [[nodiscard]] constexpr std::string_view name() const noexcept { return route_ ? route_->name : ""sv; }
        [[nodiscard]] constexpr channel_id id() const noexcept { return route_ ? route_->id : invalid_channel_id; }

      private:
        template <global_config Config, detail::basic_config_type Conf>
        constexpr friend channel<Config, detail::channel_policy_for<Conf>> detail::make_channel(const Conf& conf);

        constexpr channel(
                backend::configured_producer_backend<Global>& producer_backend, detail::route_state& route) noexcept :
                producer_backend_{&producer_backend}, route_{&route} {}

        backend::configured_producer_backend<Global>* producer_backend_{nullptr};
        detail::route_state* route_{nullptr};
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
    concept channel_type = requires { typename U::policy_type; } && channel_policy_type<typename U::policy_type> &&
                           std::same_as<U, channel<U::global, typename U::policy_type>>;

    using default_channel = channel<default_global_config, detail::default_channel_policy>;

    static_assert(channel_policy_type<detail::default_channel_policy>);
    static_assert(channel_type<default_channel>);

    namespace detail {

        template <global_config Global, basic_config_type Conf>
        constexpr channel<Global, channel_policy_for<Conf>> make_channel(const Conf& conf) {
            auto& producer_backend = static_cast<backend::configured_producer_backend<Global>&>(access_producer_backend(
                    Global, +[]() -> backend::runtime_producer_backend* {
                        return new backend::configured_producer_backend<Global>{};
                    }));
            auto handle = make_channel_route(
                    producer_backend,
                    channel_registration{
                            .name = conf.name,
                            .runtime_mode = config_runtime_mode_v<Conf>,
                            .huge_pages = Conf::use_huge_pages,
                            .sink_type = config_sink_type(conf),
                            .pattern = config_pattern_program(conf),
                            .color = conf.color(),
                            .time_requirements = config_format_requirements(conf),
                            .filename = config_filename(conf),
                            .output_fd = config_output_fd(conf),
                            .unix_dgram_path = config_unix_dgram_path(conf),
                    });

            return channel<Global, channel_policy_for<Conf>>{producer_backend, *handle.route};
        }

        void add_sink_route(backend::runtime_producer_backend& producer_backend, backend::sink_entry entry);

        template <global_config Global, basic_config_type Conf>
        inline void add_sink(const Conf& conf, sink_ptr sink) {
            add_sink_route(
                    access_producer_backend(
                            Global,
                            +[]() -> backend::runtime_producer_backend* {
                                return new backend::configured_producer_backend<Global>{};
                            }),
                    backend::sink_entry{
                            .sink = std::move(sink),
                            .pattern = config_pattern_program(conf),
                            .render_buffer = {},
                            .color = conf.color(),
                            .requirements = config_format_requirements(conf),
                    });
        }

        void flush_backend(backend::runtime_producer_backend& producer_backend);

        void start_backend(backend::runtime_producer_backend& producer_backend);
        void prewarm_backend(backend::runtime_producer_backend& producer_backend);

#if UNLOG_DIAGNOSTIC
        backend::producer_stats backend_stats(backend::runtime_producer_backend& producer_backend);
#endif

        log_level get_global_level(backend::runtime_producer_backend& producer_backend);

        void set_global_level(backend::runtime_producer_backend& producer_backend, log_level level);

    }  // namespace detail

    template <global_config Global>
    struct configured {
        static_assert(std::has_single_bit(Global.max_record_size));
        static_assert(Global.max_record_size > sizeof(backend::record_slot_header));
        static_assert(std::has_single_bit(Global.thread_bufsize));
        static_assert(std::has_single_bit(Global.huge_thread_bufsize));
        static_assert(Global.thread_bufsize >= Global.max_record_size);
        static_assert(Global.huge_thread_bufsize >= options::default_huge_thread_bufsize);
        static_assert(Global.thread_bufsize % Global.max_record_size == 0);
        static_assert(Global.huge_thread_bufsize % Global.max_record_size == 0);
        static_assert(Global.huge_thread_bufsize % options::default_huge_thread_bufsize == 0);
        static_assert(std::has_single_bit(Global.huge_thread_bufsize / options::default_huge_thread_bufsize));
        static_assert(std::has_single_bit(Global.max_producers));

        static constexpr auto config = Global;
        using producer_backend_type = backend::configured_producer_backend<Global>;

        template <detail::basic_config_type Conf>
        static constexpr auto make_channel(const Conf& conf) {
            return detail::make_channel<Global>(conf);
        }

        // Completes producer construction and route activation. It must return before
        // enabled logging begins concurrently on other threads.
        static void start() { detail::start_backend(producer_backend()); }
        static void prewarm_thread() { detail::prewarm_backend(producer_backend()); }
        static void flush() { detail::flush_backend(producer_backend()); }
        static void set_global_level(log_level level = log_level::info) {
            detail::set_global_level(producer_backend(), level);
        }
        static log_level get_global_level() { return detail::get_global_level(producer_backend()); }

#if UNLOG_DIAGNOSTIC
        static backend::producer_stats stats() { return detail::backend_stats(producer_backend()); }
#endif

      private:
        static producer_backend_type& producer_backend() {
            return static_cast<producer_backend_type&>(detail::access_producer_backend(
                    Global, +[]() -> backend::runtime_producer_backend* { return new producer_backend_type{}; }));
        }
    };

}  // namespace un::log
