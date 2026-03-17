#include "unlog.hpp"

#include "unlog/backend/sinks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
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
#include <tuple>
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

        using runtime_queue_producer = backend::runtime_queue_producer;

        struct queue_producer_registry {
            std::mutex mutex;
            std::vector<std::unique_ptr<runtime_queue_producer>> producers;
            std::vector<std::unique_ptr<backend::producer_active_word>> active_words;
        };

        enum class runtime_lifecycle : uint8_t { uninitialized, initialized, shutting_down, shut_down };

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
            std::shared_ptr<std::vector<backend::sink_entry>> consumer_sinks;
            std::condition_variable consumer_cv;
            std::thread consumer_thread;
            bool consumer_started{false};
            bool stop_requested{false};
            std::atomic<uint64_t> flush_requested{0};
            std::atomic<uint64_t> flush_completed{0};
            std::unique_ptr<backend::runtime_queue_producer> single_threaded_producer;
            queue_producer_registry producer_registry;
        };

        struct runtime_control {
            std::mutex mutex;
            std::atomic<runtime_state*> state{nullptr};
            std::atomic<runtime_lifecycle> lifecycle{runtime_lifecycle::uninitialized};
            std::atomic<uint64_t> generation{0};
            bool process_exit_hook_registered{false};
        };

        static runtime_control& access_control() {
            static runtime_control control;
            return control;
        }

        static runtime_state* try_access_state() noexcept {
            return access_control().state.load(std::memory_order_acquire);
        }

        static runtime_state& access_state() {
            auto* state = try_access_state();
            if (!state) {
                throw std::logic_error{"runtime state is not initialized"};
            }

            return *state;
        }

        static runtime_state& ensure_state_created_locked(runtime_control& control) {
            auto lifecycle = control.lifecycle.load(std::memory_order_acquire);
            if (lifecycle == runtime_lifecycle::shutting_down || lifecycle == runtime_lifecycle::shut_down) {
                throw std::invalid_argument{"runtime is shut down; cannot reinitialize"};
            }

            auto* state = control.state.load(std::memory_order_acquire);
            if (!state) {
                state = new runtime_state{};
                control.state.store(state, std::memory_order_release);
                control.lifecycle.store(runtime_lifecycle::initialized, std::memory_order_release);
                control.generation.fetch_add(1, std::memory_order_acq_rel);
            }

            return *state;
        }

        static void shutdown_runtime_state(runtime_state& runtime) noexcept {
            if (!runtime.consumer_started) {
                return;
            }

            {
                std::lock_guard lock{runtime.runtime_mutex};
                runtime.stop_requested = true;
            }

            runtime.consumer_cv.notify_all();
            if (runtime.consumer_thread.joinable()) {
                runtime.consumer_thread.join();
            }

            runtime.consumer_started = false;
            runtime.stop_requested = false;
        }

        static void destroy_runtime(runtime_lifecycle final_lifecycle) noexcept {
            auto& control = access_control();
            auto* state = static_cast<runtime_state*>(nullptr);

            {
                std::lock_guard lock{control.mutex};
                auto lifecycle = control.lifecycle.load(std::memory_order_acquire);
                if (lifecycle == runtime_lifecycle::shutting_down) {
                    return;
                }

                state = control.state.load(std::memory_order_acquire);
                if (!state) {
                    control.lifecycle.store(final_lifecycle, std::memory_order_release);
                    return;
                }

                control.lifecycle.store(runtime_lifecycle::shutting_down, std::memory_order_release);
            }

            shutdown_runtime_state(*state);

            {
                std::lock_guard lock{control.mutex};
                control.state.store(nullptr, std::memory_order_release);
                control.lifecycle.store(final_lifecycle, std::memory_order_release);
            }

            delete state;
        }

        static void shutdown_runtime_for_process_exit() noexcept {
            destroy_runtime(runtime_lifecycle::shut_down);
        }

        static void register_process_exit_hook_locked(runtime_control& control) {
            if (control.process_exit_hook_registered) {
                return;
            }

            std::atexit(&shutdown_runtime_for_process_exit);
            control.process_exit_hook_registered = true;
        }

        static bool runtime_is_shutting_down() noexcept {
            auto lifecycle = access_control().lifecycle.load(std::memory_order_acquire);
            return lifecycle == runtime_lifecycle::shutting_down || lifecycle == runtime_lifecycle::shut_down;
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

            std::lock_guard runtime_lock{st.runtime_mutex};
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
            auto snapshot = std::make_shared<std::vector<backend::sink_entry>>();
            snapshot->reserve(st.configured_sinks.size() + st.custom_sinks.size());
            std::ranges::copy(
                    st.configured_sinks | std::views::transform([](const auto& sink) { return sink.entry; }),
                    std::back_inserter(*snapshot));
            std::ranges::copy(st.custom_sinks, std::back_inserter(*snapshot));
            st.consumer_sinks = std::move(snapshot);
        }

        static uint64_t current_thread_id() {
            return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

        static void reconfigure_runtime_queue_producers(runtime_state& st, size_t thread_bufsize) {
            if (st.single_threaded_producer) {
                st.single_threaded_producer->reconfigure(thread_bufsize);
            }

            std::lock_guard lock{st.producer_registry.mutex};
            for (auto& producer : st.producer_registry.producers) {
                producer->reconfigure(thread_bufsize);
            }
        }

        static void ensure_global_config_locked(runtime_state& st) {
            if (st.global_config_locked) {
                return;
            }

            validate_global_config(st.global);
            if (st.global.mode == RuntimeMode::single_threaded && !st.single_threaded_producer) {
                st.single_threaded_producer =
                        std::make_unique<runtime_queue_producer>(current_thread_id(), st.global.thread_bufsize);
            }
            reconfigure_runtime_queue_producers(st, st.global.thread_bufsize);
            st.global_config_locked = true;
        }

        static runtime_queue_producer& register_runtime_queue_producer(runtime_state& st) {
            std::lock_guard lock{st.producer_registry.mutex};
            auto producer_index = st.producer_registry.producers.size();
            auto required_word_count = producer_index / size_t{64} + size_t{1};
            while (st.producer_registry.active_words.size() < required_word_count) {
                st.producer_registry.active_words.push_back(std::make_unique<backend::producer_active_word>());
            }

            auto producer = std::make_unique<runtime_queue_producer>(current_thread_id(), st.global.thread_bufsize);
            producer->bind_active_word(
                    st.producer_registry.active_words[producer_index / size_t{64}].get(), producer_index);
            st.producer_registry.producers.push_back(std::move(producer));
            return *st.producer_registry.producers.back();
        }

        static runtime_queue_producer& single_threaded_runtime_queue_producer() {
            auto& st = access_state();
            if (!st.single_threaded_producer) {
                st.single_threaded_producer =
                        std::make_unique<runtime_queue_producer>(current_thread_id(), st.global.thread_bufsize);
            }

            return *st.single_threaded_producer;
        }

        struct tls_runtime_queue_state {
            runtime_state* owner{nullptr};
            uint64_t generation{0};
            runtime_queue_producer* producer{nullptr};
        };

        static runtime_queue_producer& ensure_threadsafe_runtime_queue_producer(runtime_state& st) {
            static thread_local tls_runtime_queue_state tls_state;
            auto generation = access_control().generation.load(std::memory_order_acquire);
            if (tls_state.owner != &st || tls_state.generation != generation || !tls_state.producer) {
                tls_state.owner = &st;
                tls_state.generation = generation;
                tls_state.producer = &register_runtime_queue_producer(st);
            }

            return *tls_state.producer;
        }

        backend::runtime_queue_producer& get_runtime_queue_producer(RuntimeMode runtime_mode) {
            auto& st = access_state();
            if (runtime_mode == RuntimeMode::single_threaded) {
                return single_threaded_runtime_queue_producer();
            }

            return ensure_threadsafe_runtime_queue_producer(st);
        }

#if UNLOG_DIAGNOSTIC
        static std::vector<runtime_queue_producer*> runtime_queue_producer_snapshot() {
            auto* st = try_access_state();
            if (!st) {
                return {};
            }

            if (st->global.mode == RuntimeMode::single_threaded) {
                if (!st->single_threaded_producer) {
                    return {};
                }

                return {st->single_threaded_producer.get()};
            }

            std::lock_guard lock{st->producer_registry.mutex};
            return st->producer_registry.producers |
                   std::views::transform([](const auto& producer) { return producer.get(); }) |
                   std::ranges::to<std::vector>();
        }
#endif

        static std::vector<runtime_queue_producer*> active_runtime_queue_producer_snapshot(runtime_state& runtime) {
            std::lock_guard lock{runtime.producer_registry.mutex};

            auto snapshot = std::vector<runtime_queue_producer*>{};
            for (size_t word_index = 0; word_index < runtime.producer_registry.active_words.size(); ++word_index) {
                auto pending = runtime.producer_registry.active_words[word_index]->bits.load(std::memory_order_acquire);
                while (pending != 0) {
                    auto bit_index = static_cast<size_t>(std::countr_zero(pending));
                    auto producer_index = word_index * size_t{64} + bit_index;
                    if (producer_index < runtime.producer_registry.producers.size()) {
                        snapshot.push_back(runtime.producer_registry.producers[producer_index].get());
                    }
                    pending &= pending - uint64_t{1};
                }
            }

            return snapshot;
        }

        static bool runtime_has_pending_work(runtime_state& runtime) {
            if (runtime.global.mode == RuntimeMode::single_threaded) {
                return runtime.single_threaded_producer && !runtime.single_threaded_producer->queue().empty();
            }

            std::lock_guard lock{runtime.producer_registry.mutex};
            for (const auto& word : runtime.producer_registry.active_words) {
                if (word->bits.load(std::memory_order_acquire) != 0) {
                    return true;
                }
            }

            return false;
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
                    .message = slot.message(),
                    .timestamp = slot.header.timestamp,
            };

            if (auto* state = find_channel_state(runtime, slot.header.channel)) {
                rec.logger_name = state->name;
            }

            return rec;
        }

        struct consumer_scratch {
            std::vector<backend::line_cache_entry> line_cache;
        };

        static void rebuild_consumer_sinks_locked(runtime_state& runtime) {
            auto snapshot = std::make_shared<std::vector<backend::sink_entry>>();
            snapshot->reserve(runtime.configured_sinks.size() + runtime.custom_sinks.size());
            std::ranges::copy(
                    runtime.configured_sinks | std::views::transform([](const auto& sink) { return sink.entry; }),
                    std::back_inserter(*snapshot));
            std::ranges::copy(runtime.custom_sinks, std::back_inserter(*snapshot));
            runtime.consumer_sinks = std::move(snapshot);
        }

        static std::shared_ptr<const std::vector<backend::sink_entry>> sink_snapshot(runtime_state& runtime) {
            std::lock_guard lock{runtime.runtime_mutex};
            if (!runtime.consumer_sinks) {
                rebuild_consumer_sinks_locked(runtime);
            }

            return runtime.consumer_sinks;
        }

        static void emit_runtime_slot(
                runtime_state& runtime,
                const std::vector<backend::sink_entry>& sinks,
                consumer_scratch& scratch,
                const backend::runtime_record_slot& slot) {
            auto rec = make_runtime_log_entry(runtime, slot);
            auto time_context = resolve_runtime_time_context(rec.timestamp, runtime.clock_type);
            auto& line_cache = scratch.line_cache;
            line_cache.clear();
            if (line_cache.capacity() < sinks.size()) {
                line_cache.reserve(sinks.size());
            }

            for (const auto& sink : sinks) {
                auto line = backend::format_cache_line(
                        line_cache,
                        sink.pattern,
                        true,
                        rec,
                        time_context.tm,
                        time_context.millis,
                        time_context.elapsed);
                sink.sink->write(line);
            }
        }

        static size_t drain_runtime_queue_producer(
                runtime_state& runtime,
                const std::vector<backend::sink_entry>& sinks,
                consumer_scratch& scratch,
                runtime_queue_producer& producer) {
            auto drained_total = size_t{0};

            for (;;) {
                auto drained_pass = producer.queue().consume_all(
                        [&](backend::runtime_record_slot& slot) { emit_runtime_slot(runtime, sinks, scratch, slot); });
                drained_total += drained_pass;
                if (drained_pass != 0) {
                    continue;
                }

                producer.clear_active_published();
                if (auto* active_word = producer.active_word()) {
                    active_word->bits.fetch_and(~producer.active_bit_mask(), std::memory_order_acq_rel);
                }

                if (!producer.queue().empty()) {
                    if (producer.try_mark_active_published()) {
                        if (auto* active_word = producer.active_word()) {
                            active_word->bits.fetch_or(producer.active_bit_mask(), std::memory_order_release);
                        }
                    }
                    continue;
                }

                return drained_total;
            }
        }

        static size_t drain_runtime_queues(runtime_state& runtime, consumer_scratch& scratch) {
            auto drained_total = size_t{0};

            for (;;) {
                auto sinks = sink_snapshot(runtime);

                if (runtime.global.mode == RuntimeMode::single_threaded) {
                    if (!runtime.single_threaded_producer) {
                        return drained_total;
                    }

                    auto drained_pass =
                            drain_runtime_queue_producer(runtime, *sinks, scratch, *runtime.single_threaded_producer);
                    drained_total += drained_pass;
                    if (drained_pass == 0) {
                        return drained_total;
                    }
                    continue;
                }

                auto producer_snapshot = active_runtime_queue_producer_snapshot(runtime);
                if (producer_snapshot.empty()) {
                    return drained_total;
                }

                auto drained_pass = size_t{0};
                for (auto* producer : producer_snapshot) {
                    if (!producer) {
                        continue;
                    }

                    drained_pass += drain_runtime_queue_producer(runtime, *sinks, scratch, *producer);
                }

                drained_total += drained_pass;
            }
        }

        static void flush_runtime_sinks(runtime_state& runtime) {
            auto sinks = sink_snapshot(runtime);
            for (const auto& sink : *sinks) {
                sink.sink->flush();
            }
        }

        static void consumer_main(runtime_state& runtime) {
            auto scratch = consumer_scratch{};

            for (;;) {
                drain_runtime_queues(runtime, scratch);

                auto requested = runtime.flush_requested.load(std::memory_order_acquire);
                if (requested != runtime.flush_completed.load(std::memory_order_acquire)) {
                    drain_runtime_queues(runtime, scratch);
                    flush_runtime_sinks(runtime);
                    runtime.flush_completed.store(requested, std::memory_order_release);
                    runtime.consumer_cv.notify_all();
                    continue;
                }

                std::unique_lock lock{runtime.runtime_mutex};
                if (runtime.stop_requested) {
                    break;
                }

                runtime.consumer_cv.wait(lock, [&runtime] {
                    return runtime.stop_requested ||
                           runtime.flush_requested.load(std::memory_order_acquire) !=
                                   runtime.flush_completed.load(std::memory_order_acquire) ||
                           runtime_has_pending_work(runtime);
                });
            }

            drain_runtime_queues(runtime, scratch);
            flush_runtime_sinks(runtime);
        }

        static void ensure_consumer_started_locked(runtime_state& runtime) {
            if (runtime.consumer_started) {
                return;
            }

            runtime.stop_requested = false;
            // std::thread stores args by value after decay, so use std::ref
            runtime.consumer_thread = std::thread{consumer_main, std::ref(runtime)};
            runtime.consumer_started = true;
        }

        static void ensure_runtime_initialized_locked(runtime_state& st) {
            ensure_global_config_locked(st);
            ensure_consumer_started_locked(st);
            if (st.global.mode == RuntimeMode::threadsafe) {
                std::ignore = ensure_threadsafe_runtime_queue_producer(st);
            }
        }

        void mark_runtime_active_after_commit() noexcept {
            if (auto* st = try_access_state()) {
                auto expected = false;
                if (st->is_active.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                    st->consumer_cv.notify_one();
                }
            }
        }

        void note_runtime_work_available(runtime_queue_producer& producer, RuntimeMode runtime_mode) noexcept {
            if (!producer.try_mark_active_published()) {
                return;
            }

            if (auto* st = try_access_state()) {
                if (runtime_mode == RuntimeMode::threadsafe) {
                    if (auto* active_word = producer.active_word()) {
                        active_word->bits.fetch_or(producer.active_bit_mask(), std::memory_order_release);
                    }
                }
                st->consumer_cv.notify_one();
            }
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

            std::ignore = make_channel_route_locked(
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
            if (auto* st = try_access_state()) {
                return st->default_level;
            }

            return log_level::info;
        }

        void set_default_level(log_level level) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);

            st.default_level = level;

            for (auto& route : st.channels) {
                route.set_level(level);
            }
        }

        channel make_channel_route(channel_registration registration, bool make_default) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);
            return make_channel_route_locked(st, std::move(registration), make_default, false);
        }

        void add_sink_route(sink_ptr sink, ClockType timestamp_mode, std::string_view format) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);

            if (st.is_active.load(std::memory_order_relaxed)) {
                throw std::invalid_argument{"runtime is active; cannot add sinks"};
            }
            if (!sink) {
                throw std::invalid_argument{"add_sink requires a non-null sink"};
            }

            ensure_global_config_locked(st);
            set_runtime_clock_type(st, timestamp_mode);

            std::lock_guard runtime_lock{st.runtime_mutex};
            st.custom_sinks.push_back(
                    backend::sink_entry{
                            .sink = std::move(sink),
                            .pattern = std::string{format},
                    });
            rebuild_consumer_sinks_locked(st);
        }

        void flush_backend() {
            auto* st = try_access_state();
            if (!st || !st->consumer_started) {
                return;
            }

            auto target = st->flush_requested.fetch_add(1, std::memory_order_acq_rel) + 1u;
            st->consumer_cv.notify_one();

            std::unique_lock lock{st->runtime_mutex};
            st->consumer_cv.wait(
                    lock, [st, target] { return st->flush_completed.load(std::memory_order_acquire) >= target; });
        }

#if UNLOG_DIAGNOSTIC
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
#endif

        channel_runtime_view channel_runtime_view_for(channel_id id) noexcept {
            if (runtime_is_shutting_down()) {
                return {};
            }

            auto* st = try_access_state();
            if (!st) {
                return {};
            }

            auto* state = find_channel_state(*st, id);
            if (!state) {
                return {};
            }

            auto view = state->runtime_view();
            view.runtime_mode = st->global.mode;
            view.clock_now_fn = st->clock_now_fn;
            return view;
        }

        void set_channel_level(channel_id id, log_level level) noexcept {
            if (auto* st = try_access_state()) {
                if (auto* state = find_channel_state(*st, id)) {
                    state->set_level(level);
                }
            }
        }

        log_level channel_level(channel_id id) noexcept {
            if (auto* st = try_access_state()) {
                if (auto* state = find_channel_state(*st, id)) {
                    return state->level();
                }
            }

            return log_level::off;
        }

    }  // namespace detail

    namespace test {
        void get_runtime_backend(const std::function<void()>& fn) {
            if (!fn) {
                return;
            }

            auto* st = detail::try_access_state();
            if (!st) {
                return;
            }

            auto ready = st->consumer_started || !st->channels.empty();
            if (!ready) {
                std::lock_guard lock{st->runtime_mutex};
                ready = !st->configured_sinks.empty() || !st->custom_sinks.empty();
            }

            if (!ready) {
                return;
            }

            fn();
        }

        bool consumer_thread_started() {
            if (auto* st = detail::try_access_state()) {
                return st->consumer_started;
            }

            return false;
        }

        void reset_runtime_for_test() {
            detail::destroy_runtime(detail::runtime_lifecycle::uninitialized);
        }

        size_t threadsafe_producer_count() {
            auto* st = detail::try_access_state();
            if (!st || st->global.mode != RuntimeMode::threadsafe) {
                return 0;
            }

            std::lock_guard lock{st->producer_registry.mutex};
            return st->producer_registry.producers.size();
        }
    }  // namespace test

    channel global_channel() {
        if (detail::runtime_is_shutting_down()) {
            return {};
        }

        auto& control = detail::access_control();
        std::lock_guard lock{control.mutex};
        auto& st = detail::ensure_state_created_locked(control);
        detail::register_process_exit_hook_locked(control);
        detail::ensure_default_channel(st);

        if (!st.default_channel_id.has_value()) {
            return {};
        }

        return detail::make_channel_handle(st, *st.default_channel_id);
    }

    void set_global_config(global_config cfg) {
        auto& control = detail::access_control();
        std::lock_guard lock{control.mutex};
        auto& st = detail::ensure_state_created_locked(control);
        detail::register_process_exit_hook_locked(control);

        if (st.global_config_locked || st.is_active.load(std::memory_order_relaxed)) {
            throw std::invalid_argument{"global config is locked; cannot reconfigure"};
        }

        detail::validate_global_config(cfg);
        st.global = cfg;
        detail::reconfigure_runtime_queue_producers(st, cfg.thread_bufsize);
    }

    global_config get_global_config() {
        if (auto* st = detail::try_access_state()) {
            std::lock_guard lock{st->runtime_mutex};
            return st->global;
        }

        return {};
    }

    void prewarm_thread() {
        if (detail::runtime_is_shutting_down()) {
            return;
        }

        auto& control = detail::access_control();
        std::lock_guard lock{control.mutex};
        auto* st = control.state.load(std::memory_order_acquire);
        if (!st) {
            return;
        }

        if (st->global.mode != RuntimeMode::threadsafe) {
            return;
        }

        std::ignore = detail::ensure_threadsafe_runtime_queue_producer(*st);
    }

    void flush() {
        detail::flush_backend();
    }

}  // namespace un::log
