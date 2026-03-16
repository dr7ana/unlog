#include "unlog.hpp"

#include "unlog/backend/sinks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "backend/internal.hpp"

namespace un::log {
    namespace detail {
        // Clock constraint for timestamp capture, aligned with the C++ named requirement TrivialClock.
        template <typename Clock>
        concept log_clock = requires {
            typename Clock::time_point;
            { Clock::now() } noexcept -> std::same_as<typename Clock::time_point>;
            { Clock::is_steady } -> std::convertible_to<const bool&>;
        };

        template <log_clock Clock>
        static constexpr uint64_t clock_now_ns() noexcept {
            auto now = Clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        }

        template <ClockType Type>
        static constexpr uint64_t clock_now_ns_for() noexcept {
            if constexpr (Type == ClockType::system)
                return clock_now_ns<std::chrono::system_clock>();

            return clock_now_ns<std::chrono::steady_clock>();
        }

        template <ClockType Type>
        static constexpr clock_now_fn_t clock_now_fn_for() noexcept {
            return &clock_now_ns_for<Type>;
        }

        static constexpr clock_now_fn_t clock_now_fn_for(ClockType type) noexcept {
            if (type == ClockType::system) {
                return &clock_now_ns_for<ClockType::system>;
            }

            return &clock_now_ns_for<ClockType::steady>;
        }

        struct channel_state {
            channel_state(std::string channel_name, size_t max_message_size, bool overflow_drop, bool can_truncate) :
                    name{std::move(channel_name)},
                    max_message_size{max_message_size},
                    overflow_drop{overflow_drop},
                    can_truncate{can_truncate} {}

            void set_level(log_level level) noexcept { level_.store(level, std::memory_order_relaxed); }

            [[nodiscard]] log_level level() const noexcept { return level_.load(std::memory_order_relaxed); }

            [[nodiscard]] channel_runtime_view runtime_view() const noexcept {
                return channel_runtime_view{
                        .channel_name = name.c_str(),
                        .max_message_size = max_message_size,
                        .overflow_drop = overflow_drop,
                        .can_truncate = can_truncate,
                        .threshold = level(),
                };
            }

            std::string name;
            size_t max_message_size{0};
            bool overflow_drop{true};
            bool can_truncate{false};

          private:
            std::atomic<log_level> level_{log_level::info};
        };

        struct configured_sink {
            backend::sink_entry entry;
            SinkType sink_type{SinkType::cout};
            std::string target_key;
        };

        struct runtime_state {
            std::unordered_map<std::string, channel_id> channel_ids;
            std::deque<channel_state> channels;
            std::optional<channel_id> default_channel_id;
            global_config global{};
            bool global_config_locked{false};
            ClockType clock_type{ClockType::steady};
            bool clock_type_set{false};
            clock_now_fn_t clock_now_fn{clock_now_fn_for<ClockType::steady>()};
            log_level default_level{log_level::info};
            // TODO(multithread-hardening): if setup/logging can run concurrently at high thread counts,
            // revisit this gate with explicit acquire/release ordering and stronger transition semantics.
            std::atomic<bool> is_active{false};
            std::mutex runtime_mutex;
            std::vector<configured_sink> configured_sinks;
            std::vector<backend::sink_entry> custom_sinks;
            std::condition_variable consumer_cv;
            std::thread consumer_thread;
            bool consumer_started{false};
            bool stop_requested{false};
            std::atomic<uint64_t> work_generation{0};
            std::atomic<uint64_t> flush_requested{0};
            std::atomic<uint64_t> flush_completed{0};
            std::unique_ptr<backend::runtime_queue_producer> single_threaded_producer;
        };

        static runtime_state& access_state() {
            static runtime_state instance;
            return instance;
        }

        static channel_state* find_channel_state(runtime_state& st, channel_id id) noexcept {
            auto index = static_cast<size_t>(id);
            if (id == invalid_channel_id || index >= st.channels.size()) {
                return nullptr;
            }
            return &st.channels[index];
        }

        static const channel_state* find_channel_state(const runtime_state& st, channel_id id) noexcept {
            auto index = static_cast<size_t>(id);
            if (id == invalid_channel_id || index >= st.channels.size()) {
                return nullptr;
            }
            return &st.channels[index];
        }

        static channel make_channel_handle(const runtime_state& st, channel_id id) noexcept {
            auto* state = find_channel_state(st, id);
            if (!state) {
                return {};
            }

            return channel{id, state->name};
        }

        static void set_runtime_clock_type(runtime_state& st, ClockType type) {
            if (!st.clock_type_set) {
                st.clock_type = type;
                st.clock_type_set = true;
                st.clock_now_fn = clock_now_fn_for(type);
                return;
            }

            if (st.clock_type != type) {
                throw std::invalid_argument{"cannot mix different backend clock types in one runtime"};
            }
        }

        static void validate_global_config(const global_config& cfg) {
            if (!backend::runtime_queue_traits::queue_capacity_for(cfg.thread_bufsize).has_value()) {
                throw std::invalid_argument{
                        "global thread_bufsize does not provide enough capacity for one producer record slot"};
            }
        }

        static std::string sink_target_key(
                SinkType sink_type,
                std::string_view pattern,
                const std::optional<fs::path>& filename,
                const std::optional<int>& output_fd,
                const std::optional<fs::path>& unix_dgram_path) {
            auto key = std::string{sink_type_string(sink_type)};
            key.push_back('|');
            key.append(pattern);
            key.push_back('|');

            switch (sink_type) {
                case SinkType::cout:
                case SinkType::cerr:
                    break;
                case SinkType::fd:
                    if (output_fd.has_value()) {
                        key.append(std::to_string(*output_fd));
                    }
                    break;
                case SinkType::file:
                    if (filename.has_value()) {
                        key.append(filename->string());
                    }
                    break;
                case SinkType::unix_dgram:
                    if (unix_dgram_path.has_value()) {
                        key.append(unix_dgram_path->string());
                    }
                    break;
                default:
                    break;
            }

            return key;
        }

        static backend::sink_ptr make_configured_sink(
                SinkType sink_type,
                const std::optional<fs::path>& filename,
                const std::optional<int>& output_fd,
                const std::optional<fs::path>& unix_dgram_path) {
            switch (sink_type) {
                case SinkType::cout:
                    return std::make_shared<backend::ostream_sink>(std::cout, true);
                case SinkType::cerr:
                    return std::make_shared<backend::ostream_sink>(std::cerr, true);
                case SinkType::fd:
                    if (!output_fd.has_value()) {
                        throw std::invalid_argument{"fd sink requires output_fd"};
                    }
                    return std::make_shared<backend::fd_sink>(*output_fd);
                case SinkType::file:
                    if (!filename.has_value() || filename->empty()) {
                        throw std::invalid_argument{"file sink requires filename"};
                    }
                    return std::make_shared<backend::file_sink>(filename->string());
                case SinkType::unix_dgram:
                    if (!unix_dgram_path.has_value()) {
                        throw std::invalid_argument{"unix_dgram sink requires unix_dgram_path"};
                    }
                    return std::make_shared<backend::unix_dgram_sink>(unix_dgram_path->string());
                default:
                    throw std::invalid_argument{"unsupported sink type"};
            }
        }

        static void add_configured_sink_locked(runtime_state& st, const channel_registration& registration) {
            auto key = sink_target_key(
                    registration.sink_type,
                    registration.format,
                    registration.filename,
                    registration.output_fd,
                    registration.unix_dgram_path);

            auto duplicate = std::ranges::find_if(
                    st.configured_sinks, [&key](const configured_sink& sink) { return sink.target_key == key; });
            if (duplicate != st.configured_sinks.end()) {
                return;
            }

            st.configured_sinks.push_back(
                    configured_sink{
                            .entry =
                                    backend::sink_entry{
                                            .sink = make_configured_sink(
                                                    registration.sink_type,
                                                    registration.filename,
                                                    registration.output_fd,
                                                    registration.unix_dgram_path),
                                            .pattern = std::string{registration.format},
                                    },
                            .sink_type = registration.sink_type,
                            .target_key = std::move(key),
                    });
        }

        using runtime_queue_producer = backend::runtime_queue_producer;

        struct queue_producer_registry {
            std::mutex mutex;
            std::vector<std::unique_ptr<runtime_queue_producer>> producers;
        };

        static queue_producer_registry& runtime_queue_registry() {
            static queue_producer_registry registry;
            return registry;
        }

        static uint64_t current_thread_id() {
            return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

        static void reconfigure_runtime_queue_producers(size_t thread_bufsize) {
            auto& st = access_state();
            if (st.single_threaded_producer) {
                st.single_threaded_producer->reconfigure(thread_bufsize);
            }

            auto& registry = runtime_queue_registry();
            std::lock_guard lock{registry.mutex};
            for (auto& producer : registry.producers) {
                producer->reconfigure(thread_bufsize);
            }
        }

        static void ensure_global_config_locked(runtime_state& st) {
            if (st.global_config_locked) {
                return;
            }

            validate_global_config(st.global);
            reconfigure_runtime_queue_producers(st.global.thread_bufsize);
            st.global_config_locked = true;
        }

        static runtime_queue_producer& register_runtime_queue_producer() {
            auto& st = access_state();
            auto& registry = runtime_queue_registry();
            std::lock_guard lock{registry.mutex};
            registry.producers.push_back(
                    std::make_unique<runtime_queue_producer>(current_thread_id(), st.global.thread_bufsize));
            return *registry.producers.back();
        }

        static runtime_queue_producer& single_threaded_runtime_queue_producer() {
            auto& st = access_state();
            if (!st.single_threaded_producer) {
                st.single_threaded_producer =
                        std::make_unique<runtime_queue_producer>(current_thread_id(), st.global.thread_bufsize);
            }

            return *st.single_threaded_producer;
        }

        static std::vector<runtime_queue_producer*> runtime_queue_producer_snapshot() {
            auto& st = access_state();
            if (st.global.mode == RuntimeMode::single_threaded) {
                return {&single_threaded_runtime_queue_producer()};
            }

            auto& registry = runtime_queue_registry();
            std::lock_guard lock{registry.mutex};
            return registry.producers | std::views::transform([](auto&& producer) { return producer.get(); }) |
                   std::ranges::to<std::vector>();
        }

        struct tls_runtime_queue_state {
            tls_runtime_queue_state() : producer{&register_runtime_queue_producer()} {}

            runtime_queue_producer* producer{nullptr};
        };

        backend::runtime_queue_producer& get_runtime_queue_producer(RuntimeMode runtime_mode) {
            if (runtime_mode == RuntimeMode::single_threaded) {
                return single_threaded_runtime_queue_producer();
            }

            static thread_local tls_runtime_queue_state tls_state;
            return *tls_state.producer;
        }

        static void reset_runtime_queue_producers_for_test() {
            auto& st = access_state();
            if (st.single_threaded_producer) {
                st.single_threaded_producer->reset_for_test();
            }

            auto& registry = runtime_queue_registry();
            std::lock_guard lock{registry.mutex};
            for (auto& producer : registry.producers) {
                producer->reset_for_test();
            }
        }

        auto runtime_startup_steady_time = std::chrono::steady_clock::now();
        auto runtime_startup_system_time = std::chrono::system_clock::now();
        auto runtime_startup_steady_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(runtime_startup_steady_time.time_since_epoch());
        auto runtime_startup_system_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(runtime_startup_system_time.time_since_epoch());

        static backend::time_context resolve_runtime_time_context(uint64_t timestamp, ClockType clock_type) {
            auto sample_system = std::chrono::system_clock::time_point{};
            auto elapsed = std::chrono::nanoseconds{0};

            if (clock_type == ClockType::system) {
                auto system_ticks = backend::ticks_to_ns(timestamp);
                sample_system = std::chrono::system_clock::time_point{system_ticks};
                elapsed = system_ticks - runtime_startup_system_ns;
            }
            else {
                auto steady_ticks = backend::ticks_to_ns(timestamp);
                elapsed = steady_ticks - runtime_startup_steady_ns;
                sample_system = runtime_startup_system_time + elapsed;
            }

            return backend::time_context{
                    .tm = backend::local_time(sample_system),
                    .millis = backend::millis_part(sample_system),
                    .elapsed = backend::format_elapsed(elapsed),
            };
        }

        static backend::log_entry make_runtime_log_entry(
                runtime_state& runtime, const backend::runtime_record_slot& slot) {
            auto rec = backend::log_entry{
                    .logger_name = {},
                    .level = slot.level(),
                    .source_location = slot.source_location(),
                    .message = std::string{slot.message()},
                    .timestamp = slot.header.timestamp,
            };

            if (auto* state = find_channel_state(runtime, slot.header.channel)) {
                rec.logger_name = state->name;
            }

            return rec;
        }

        static std::vector<backend::sink_entry> sink_snapshot(runtime_state& runtime) {
            std::lock_guard lock{runtime.runtime_mutex};
            auto snapshot = runtime.configured_sinks |
                            std::views::transform([](const auto& sink) { return sink.entry; }) |
                            std::ranges::to<std::vector>();
            std::ranges::copy(runtime.custom_sinks, std::back_inserter(snapshot));
            return snapshot;
        }

        static void emit_runtime_slot(runtime_state& runtime, const backend::runtime_record_slot& slot) {
            auto rec = make_runtime_log_entry(runtime, slot);
            auto time_context = resolve_runtime_time_context(rec.timestamp, runtime.clock_type);
            auto sinks = sink_snapshot(runtime);

            std::vector<backend::line_cache_entry> line_cache;
            line_cache.reserve(sinks.size());

            for (auto& sink : sinks) {
                auto line = backend::format_cache_line(
                        line_cache,
                        sink.pattern,
                        false,
                        rec,
                        time_context.tm,
                        time_context.millis,
                        time_context.elapsed);
                sink.sink->write(line);
                sink.sink->write("\n"sv);
            }
        }

        static size_t drain_runtime_queues(runtime_state& runtime) {
            auto drained_total = size_t{0};

            for (;;) {
                auto producer_snapshot = runtime_queue_producer_snapshot();
                auto drained_pass = size_t{0};

                for (auto* producer : producer_snapshot) {
                    if (!producer) {
                        continue;
                    }

                    drained_pass += producer->queue().consume_all(
                            [&runtime](backend::runtime_record_slot& slot) { emit_runtime_slot(runtime, slot); });
                }

                drained_total += drained_pass;
                if (drained_pass == 0) {
                    return drained_total;
                }
            }
        }

        static void flush_runtime_sinks(runtime_state& runtime) {
            auto sinks = std::vector<backend::sink_ptr>{};
            {
                std::lock_guard lock{runtime.runtime_mutex};
                sinks = runtime.configured_sinks |
                        std::views::transform([](const auto& sink) { return sink.entry.sink; }) |
                        std::ranges::to<std::vector>();
                std::ranges::copy(
                        runtime.custom_sinks | std::views::transform([](const auto& sink) { return sink.sink; }),
                        std::back_inserter(sinks));
            }

            for (auto& sink : sinks) {
                sink->flush();
            }
        }

        static void consumer_main(runtime_state& runtime) {
            auto observed_work = runtime.work_generation.load(std::memory_order_acquire);

            for (;;) {
                drain_runtime_queues(runtime);

                auto requested = runtime.flush_requested.load(std::memory_order_acquire);
                if (requested != runtime.flush_completed.load(std::memory_order_acquire)) {
                    flush_runtime_sinks(runtime);
                    runtime.flush_completed.store(requested, std::memory_order_release);
                    runtime.consumer_cv.notify_all();
                    continue;
                }

                std::unique_lock lock{runtime.runtime_mutex};
                if (runtime.stop_requested) {
                    break;
                }

                runtime.consumer_cv.wait(lock, [&runtime, &observed_work] {
                    return runtime.stop_requested ||
                           runtime.flush_requested.load(std::memory_order_acquire) !=
                                   runtime.flush_completed.load(std::memory_order_acquire) ||
                           runtime.work_generation.load(std::memory_order_acquire) != observed_work;
                });
                observed_work = runtime.work_generation.load(std::memory_order_acquire);
            }

            drain_runtime_queues(runtime);
            flush_runtime_sinks(runtime);
        }

        static void ensure_consumer_started_locked(runtime_state& runtime) {
            if (runtime.consumer_started) {
                return;
            }

            runtime.stop_requested = false;
            // std::thread stores args by value after decay, so use std::ref
            runtime.consumer_thread = std::thread{consumer_main, std::ref(access_state())};
            runtime.consumer_started = true;
        }

        static void ensure_runtime_initialized_locked(runtime_state& st) {
            ensure_global_config_locked(st);
            ensure_consumer_started_locked(st);
        }

        void mark_runtime_active_after_commit() noexcept {
            auto& st = access_state();
            auto expected = false;
            if (st.is_active.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                st.consumer_cv.notify_one();
            }
        }

        void note_runtime_work_available() noexcept {
            auto& st = access_state();
            st.work_generation.fetch_add(1, std::memory_order_release);
            st.consumer_cv.notify_one();
        }

        static channel make_channel_route_locked(
                runtime_state& st, channel_registration registration, bool make_default, bool allow_after_activation) {

            if (!allow_after_activation && st.is_active.load(std::memory_order_relaxed)) {
                throw std::invalid_argument{"runtime is active; cannot register new channels"};
            }

            auto channel_name = std::string{registration.name};
            if (st.channel_ids.contains(channel_name)) {
                throw std::invalid_argument{"A channel with the name {} already exists"_format(registration.name)};
            }

            if (st.channels.size() >= static_cast<size_t>(invalid_channel_id)) {
                throw std::length_error{"channel registry exhausted"};
            }

            if (registration.max_message_size > backend::runtime_record_slot::payload_capacity) {
                throw std::invalid_argument{"channel message limit exceeds runtime queue slot payload capacity"};
            }

            set_runtime_clock_type(st, registration.timestamp_mode);
            add_configured_sink_locked(st, registration);

            auto id = static_cast<channel_id>(st.channels.size());
            st.channels.emplace_back(
                    std::move(channel_name),
                    registration.max_message_size,
                    registration.overflow_drop,
                    registration.can_truncate);

            auto& state = st.channels.back();
            state.set_level(st.default_level);
            st.channel_ids.emplace(state.name, id);

            if (make_default || !st.default_channel_id.has_value()) {
                st.default_channel_id = id;
            }

            ensure_runtime_initialized_locked(st);
            return channel{id, state.name};
        }

        static void ensure_default_channel(runtime_state& st) {
            if (st.default_channel_id.has_value()) {
                return;
            }

            auto conf = config<>::make();
            auto max_message_size =
                    detail::resolve_channel_max_message_size(static_cast<size_t>(decltype(conf)::max_record_size));

            [[maybe_unused]] auto route = make_channel_route_locked(
                    st,
                    channel_registration{
                            .name = conf.name,
                            .max_message_size = max_message_size,
                            .overflow_drop = detail::config_overflow_policy_v<decltype(conf)> == OverflowPolicy::drop,
                            .can_truncate = true,
                            .timestamp_mode = detail::config_clock_type_v<decltype(conf)>,
                            .sink_type = detail::config_sink_type(conf),
                            .format = conf.format,
                            .filename = detail::config_filename(conf),
                            .output_fd = detail::config_output_fd(conf),
                            .unix_dgram_path = detail::config_unix_dgram_path(conf),
                    },
                    true,
                    true);
        }

        log_level get_default_level() {
            auto& st = access_state();

            return st.default_level;
        }

        void set_default_level(log_level level) {
            auto& st = access_state();

            st.default_level = level;

            for (auto& route : st.channels) {
                route.set_level(level);
            }
        }

        channel make_channel_route(channel_registration registration, bool make_default) {
            auto& st = access_state();
            return make_channel_route_locked(st, std::move(registration), make_default, false);
        }

        void add_sink_route(sink_ptr sink, ClockType timestamp_mode, std::string_view format) {
            auto& st = access_state();

            if (st.is_active.load(std::memory_order_relaxed)) {
                throw std::invalid_argument{"runtime is active; cannot add sinks"};
            }
            if (!sink) {
                throw std::invalid_argument{"add_sink requires a non-null sink"};
            }

            ensure_global_config_locked(st);
            set_runtime_clock_type(st, timestamp_mode);

            std::lock_guard lock{st.runtime_mutex};
            st.custom_sinks.push_back(
                    backend::sink_entry{
                            .sink = std::move(sink),
                            .pattern = std::string{format},
                    });
        }

        void flush_backend() {
            auto& st = access_state();
            if (!st.consumer_started) {
                return;
            }

            auto target = st.flush_requested.fetch_add(1, std::memory_order_acq_rel) + 1u;
            st.consumer_cv.notify_one();

            std::unique_lock lock{st.runtime_mutex};
            st.consumer_cv.wait(
                    lock, [&st, target] { return st.flush_completed.load(std::memory_order_acquire) >= target; });
        }

        backend::producer_stats backend_stats() {
            auto out = backend::producer_stats{};
            auto producers = runtime_queue_producer_snapshot();
            for (auto* p : producers) {
                if (!p) {
                    continue;
                }

                auto snapshot = p->stats();
                out.emitted += snapshot.emitted;
                out.dropped += snapshot.dropped;
                out.truncated += snapshot.truncated;
            }
            return out;
        }

        channel_runtime_view channel_runtime_view_for(channel_id id) noexcept {
            auto& st = access_state();
            auto* state = find_channel_state(st, id);
            if (!state) {
                return {};
            }

            auto view = state->runtime_view();
            view.runtime_mode = st.global.mode;
            view.clock_now_fn = st.clock_now_fn;
            return view;
        }

        void set_channel_level(channel_id id, log_level level) noexcept {
            auto& st = access_state();
            if (auto* state = find_channel_state(st, id)) {
                state->set_level(level);
            }
        }

        log_level channel_level(channel_id id) noexcept {
            auto& st = access_state();
            if (auto* state = find_channel_state(st, id)) {
                return state->level();
            }

            return log_level::off;
        }

    }  // namespace detail

    namespace test {
        void get_runtime_backend(const std::function<void()>& fn) {
            if (!fn) {
                return;
            }

            auto& st = detail::access_state();
            auto ready = st.consumer_started || !st.channels.empty();
            if (!ready) {
                std::lock_guard lock{st.runtime_mutex};
                ready = !st.configured_sinks.empty() || !st.custom_sinks.empty();
            }

            if (!ready) {
                return;
            }

            fn();
        }

        bool consumer_thread_started() {
            auto& st = detail::access_state();
            return st.consumer_started;
        }

        void reset_runtime_for_test() {
            auto& st = detail::access_state();

            if (st.consumer_started) {
                {
                    std::lock_guard lock{st.runtime_mutex};
                    st.stop_requested = true;
                }
                st.consumer_cv.notify_all();
                if (st.consumer_thread.joinable()) {
                    st.consumer_thread.join();
                }
                st.consumer_started = false;
                st.stop_requested = false;
            }

            st.channel_ids.clear();
            st.channels.clear();
            st.default_channel_id.reset();
            {
                std::lock_guard lock{st.runtime_mutex};
                st.configured_sinks.clear();
                st.custom_sinks.clear();
            }
            st.global = {};
            st.global_config_locked = false;
            st.clock_type = ClockType::steady;
            st.clock_type_set = false;
            st.clock_now_fn = detail::clock_now_fn_for<ClockType::steady>();
            st.default_level = log_level::info;
            st.is_active.store(false, std::memory_order_relaxed);
            st.work_generation.store(0, std::memory_order_relaxed);
            st.flush_requested.store(0, std::memory_order_relaxed);
            st.flush_completed.store(0, std::memory_order_relaxed);
            detail::reconfigure_runtime_queue_producers(st.global.thread_bufsize);
            detail::reset_runtime_queue_producers_for_test();
        }
    }  // namespace test

    channel global_channel() {
        auto& st = detail::access_state();
        detail::ensure_default_channel(st);

        if (!st.default_channel_id.has_value()) {
            return {};
        }

        return detail::make_channel_handle(st, *st.default_channel_id);
    }

    void set_global_config(global_config cfg) {
        auto& st = detail::access_state();
        std::lock_guard lock{st.runtime_mutex};

        if (st.global_config_locked || st.is_active.load(std::memory_order_relaxed)) {
            throw std::invalid_argument{"global config is locked; cannot reconfigure"};
        }

        detail::validate_global_config(cfg);
        st.global = cfg;
        detail::reconfigure_runtime_queue_producers(cfg.thread_bufsize);
    }

    global_config get_global_config() {
        auto& st = detail::access_state();
        std::lock_guard lock{st.runtime_mutex};
        return st.global;
    }

    void flush() {
        detail::flush_backend();
    }

}  // namespace un::log
