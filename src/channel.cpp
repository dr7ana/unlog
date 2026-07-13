#include "unlog.hpp"

#include "unlog/backend/sinks.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
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
        struct configured_sink {
            backend::sink_ptr sink;
            std::string target_key;
        };

        enum class runtime_lifecycle : uint8_t { uninitialized, configuring, running, shutting_down, shut_down };

        struct runtime_state {
            std::unordered_map<std::string, channel_id> channel_ids;
            std::deque<route_state> channels;
            global_config global{};
            ClockType clock_type{ClockType::steady};
            std::atomic<log_level> global_level{log_level::info};
            backend::runtime_producer_backend* producer_backend{nullptr};
            // Elapsed-time anchors are per-state so %* measures from runtime creation and
            // resets with it, instead of anchoring to static-init time of this TU.
            std::chrono::steady_clock::time_point startup_steady_time{std::chrono::steady_clock::now()};
            std::chrono::system_clock::time_point startup_system_time{std::chrono::system_clock::now()};
            std::chrono::nanoseconds startup_steady_ns{
                    std::chrono::duration_cast<std::chrono::nanoseconds>(startup_steady_time.time_since_epoch())};
            std::chrono::nanoseconds startup_system_ns{
                    std::chrono::duration_cast<std::chrono::nanoseconds>(startup_system_time.time_since_epoch())};
            std::vector<configured_sink> configured_sinks;
            std::vector<backend::sink_entry> custom_sinks;
            backend::time_requirements custom_sink_requirements{backend::time_requirements::none};
            std::thread consumer_thread;
            bool consumer_started{false};
            std::atomic<bool> stop_requested{false};
            // Eventcount for consumer notification. Keep the full width so rollover is not part
            // of the handoff proof; implementations may still use an internal futex generation.
            std::atomic<uint64_t> wake_seq{0};
            std::atomic<uint64_t> flush_requested{0};
            std::atomic<uint64_t> flush_completed{0};
            std::atomic<uint32_t> active_flushers{0};
        };

        struct runtime_control {
            std::mutex mutex;
            std::atomic<runtime_state*> state{nullptr};
            std::atomic<runtime_lifecycle> lifecycle{runtime_lifecycle::uninitialized};
            bool process_exit_hook_registered{false};
        };

        static runtime_control& access_control() {
            // Immortal: never destroyed, so late log calls (TLS destructors, detached threads,
            // atexit handlers ordered after ours) always dereference live control.
            static runtime_control& control = *new runtime_control{};
            return control;
        }

        static void register_process_exit_hook_locked(runtime_control& control);

        // Retired states stay reachable forever: threads racing a test reset may still hold
        // pointers into the old state, and keeping the list rooted in an immortal vector makes
        // the retirement a leak-sanitizer-visible retention rather than a leak.
        static void retire_state_locked(runtime_state* state) {
            static std::vector<runtime_state*>& retired = *new std::vector<runtime_state*>{};
            retired.push_back(state);
        }

        static runtime_state* try_access_state() noexcept {
            return access_control().state.load(std::memory_order_acquire);
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
                control.lifecycle.store(runtime_lifecycle::configuring, std::memory_order_release);
            }

            return *state;
        }

        static void wake_consumer(runtime_state& runtime) noexcept {
            runtime.wake_seq.fetch_add(1, std::memory_order_release);
            runtime.wake_seq.notify_one();
        }

        backend::runtime_producer_backend& access_producer_backend(
                global_config config, producer_backend_factory make) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);

            if (st.producer_backend) {
                if (st.global != config) {
                    throw std::invalid_argument{"process already uses a different compile-time global configuration"};
                }
                return *st.producer_backend;
            }
            if (!make) {
                throw std::invalid_argument{"producer backend factory is null"};
            }

            st.global = config;
            st.clock_type = config.clock_type;
            st.producer_backend = make();
            st.producer_backend->bind_waker(
                    &st, +[](void* context) noexcept { wake_consumer(*static_cast<runtime_state*>(context)); });
            return *st.producer_backend;
        }

        static void shutdown_runtime_state(runtime_state& runtime) noexcept {
            if (runtime.consumer_started) {
                runtime.stop_requested.store(true, std::memory_order_release);
                wake_consumer(runtime);
                if (runtime.consumer_thread.joinable()) {
                    runtime.consumer_thread.join();
                }

                runtime.consumer_started = false;
                runtime.stop_requested.store(false, std::memory_order_relaxed);
            }

            for (auto& route : runtime.channels) {
                route.sink.sink.reset();
                std::string{}.swap(route.sink.render_buffer);
            }
            runtime.configured_sinks.clear();
            runtime.custom_sinks.clear();
            runtime.custom_sink_requirements = backend::time_requirements::none;
        }

        static void wait_for_active_flushers(runtime_state& runtime) noexcept {
            for (auto count = runtime.active_flushers.load(std::memory_order_acquire); count != 0;
                 count = runtime.active_flushers.load(std::memory_order_acquire)) {
                runtime.active_flushers.wait(count, std::memory_order_acquire);
            }
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

                // Silence producers before taking the consumer down: every log call gates on the
                // channel level, so racing threads stop enqueuing while the state stays alive.
                for (auto& channel : state->channels) {
                    channel.deactivate();
                }
            }

            wait_for_active_flushers(*state);
            shutdown_runtime_state(*state);

            {
                std::lock_guard lock{control.mutex};
                if (final_lifecycle == runtime_lifecycle::uninitialized) {
                    // Test reset: retire the old state (never deleted) so stale references held
                    // by racing threads stay valid; the next use creates a fresh state.
                    retire_state_locked(state);
                    control.state.store(nullptr, std::memory_order_release);
                }
                // Process exit keeps the state pointer intact: late log calls dereference live
                // (silenced) state instead of freed memory.
                control.lifecycle.store(final_lifecycle, std::memory_order_release);
            }
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

        static std::string sink_target_key(
                SinkType sink_type,
                const std::optional<fs::path>& filename,
                const std::optional<int>& output_fd,
                const std::optional<fs::path>& unix_dgram_path) {
            auto key = std::string{sink_type_string(sink_type)};
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
                    return std::make_shared<backend::ostream_sink_sc>(std::cout);
                case SinkType::cerr:
                    return std::make_shared<backend::ostream_sink_sc>(std::cerr);
                case SinkType::fd:
                    if (!output_fd.has_value()) {
                        throw std::invalid_argument{"fd sink requires output_fd"};
                    }
                    return std::make_shared<backend::fd_sink>(*output_fd);
                case SinkType::file:
                    if (!filename.has_value() || filename->empty()) {
                        throw std::invalid_argument{"file sink requires filename"};
                    }
                    return std::make_shared<backend::file_sink_sc>(filename->string());
                case SinkType::unix_dgram:
                    if (!unix_dgram_path.has_value()) {
                        throw std::invalid_argument{"unix_dgram sink requires unix_dgram_path"};
                    }
                    return std::make_shared<backend::unix_dgram_sink>(unix_dgram_path->string());
                default:
                    throw std::invalid_argument{"unsupported sink type"};
            }
        }

        static backend::sink_ptr configured_sink_for_locked(
                runtime_state& st, const channel_registration& registration) {
            auto key = sink_target_key(
                    registration.sink_type,
                    registration.filename,
                    registration.output_fd,
                    registration.unix_dgram_path);

            auto duplicate = std::ranges::find_if(
                    st.configured_sinks, [&key](const configured_sink& sink) { return sink.target_key == key; });
            if (duplicate != st.configured_sinks.end()) {
                return duplicate->sink;
            }

            auto sink = make_configured_sink(
                    registration.sink_type,
                    registration.filename,
                    registration.output_fd,
                    registration.unix_dgram_path);
            st.configured_sinks.push_back(configured_sink{.sink = sink, .target_key = std::move(key)});
            return sink;
        }

        template <bool NeedsWallClock, bool NeedsElapsed>
        static backend::time_context resolve_runtime_time_context(
                const runtime_state& runtime, uint64_t timestamp, ClockType clock_type) {
            auto context = backend::time_context{};

            if constexpr (!(NeedsWallClock || NeedsElapsed)) {
                return context;
            }

            if (clock_type == ClockType::system) {
                auto system_ticks = backend::ticks_to_ns(timestamp);

                if constexpr (NeedsWallClock) {
                    std::chrono::system_clock::time_point sample_system{
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(system_ticks)};

                    context.tm = backend::local_time(sample_system);
                    context.millis = backend::millis_part(sample_system);
                }

                if constexpr (NeedsElapsed) {
                    context.elapsed_size =
                            backend::format_elapsed(system_ticks - runtime.startup_system_ns, context.elapsed_storage);
                }

                return context;
            }

            auto steady_ticks = backend::ticks_to_ns(timestamp);
            auto elapsed = std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    steady_ticks - runtime.startup_steady_ns);

            if constexpr (NeedsWallClock) {
                std::chrono::system_clock::time_point sample_system = runtime.startup_system_time + elapsed;
                context.tm = backend::local_time(sample_system);
                context.millis = backend::millis_part(sample_system);
            }

            if constexpr (NeedsElapsed) {
                context.elapsed_size = backend::format_elapsed(elapsed, context.elapsed_storage);
            }

            return context;
        }

        struct consumer_scratch {
            // file-name pointer → basename suffix within the same literal. Producers stopped
            // stripping (sloc is a pass-through), so %g resolves here; source_location file
            // names are per-TU string literals, so this stays tiny and hits ~always. The
            // last-hit memo covers the common case (records arrive in bursts from one call
            // site) with a single pointer compare; the map bounds the miss cost no matter how
            // many TUs an embedder logs from.
            const char* last_filename{nullptr};
            std::string_view last_basename;
            std::unordered_map<const char*, std::string_view> basename_cache;
        };

        static std::string_view cached_source_basename(consumer_scratch& scratch, const char* filename) {
            if (!filename) {
                return {};
            }

            if (filename == scratch.last_filename) {
                return scratch.last_basename;
            }

            auto [it, inserted] = scratch.basename_cache.try_emplace(filename, std::string_view{filename});
            if (inserted) {
                if (auto p = it->second.rfind('/'); p != it->second.npos) {
                    it->second.remove_prefix(p + 1u);
                }
            }

            scratch.last_filename = filename;
            scratch.last_basename = it->second;
            return it->second;
        }

        static std::string_view resolve_cached_source_basename(void* context, const char* filename) {
            return cached_source_basename(*static_cast<consumer_scratch*>(context), filename);
        }

        static backend::log_entry make_runtime_log_entry(
                const backend::record_slot_header& header, std::string_view message) {
            auto rec = backend::log_entry{
                    .logger_name = {},
                    .level = backend::decode_level(header.level),
                    .source_location =
                            detail::source_loc{
                                    .filename = header.source_file,
                                    .line = header.source_line,
                                    .function = nullptr,
                            },
                    .message = message,
                    .timestamp = header.timestamp,
            };

            if (header.route) {
                rec.logger_name = header.route->name;
            }

            return rec;
        }

        template <bool NeedsWallClock, bool NeedsElapsed>
        static void emit_runtime_slot_for(
                runtime_state& runtime,
                consumer_scratch& scratch,
                const backend::record_slot_header& header,
                std::string_view message) {
            auto rec = make_runtime_log_entry(header, message);
            auto time_context = resolve_runtime_time_context<NeedsWallClock, NeedsElapsed>(
                    runtime, rec.timestamp, runtime.clock_type);

            auto write = [&](const backend::sink_entry& sink) {
                sink.sink->write(
                        backend::render_pattern(
                                sink,
                                rec,
                                time_context.tm,
                                time_context.millis,
                                time_context.elapsed(),
                                &scratch,
                                &resolve_cached_source_basename));
            };

            write(header.route->sink);
            for (const auto& sink : runtime.custom_sinks) {
                write(sink);
            }
        }

        static void emit_runtime_slot(
                runtime_state& runtime,
                consumer_scratch& scratch,
                const backend::record_slot_header& header,
                std::string_view message) {
            if (!header.route) {
                return;
            }

            switch (header.route->sink.requirements | runtime.custom_sink_requirements) {
                case backend::time_requirements::none:
                    emit_runtime_slot_for<false, false>(runtime, scratch, header, message);
                    return;
                case backend::time_requirements::wall_clock:
                    emit_runtime_slot_for<true, false>(runtime, scratch, header, message);
                    return;
                case backend::time_requirements::elapsed:
                    emit_runtime_slot_for<false, true>(runtime, scratch, header, message);
                    return;
                case backend::time_requirements::wall_clock_elapsed:
                    emit_runtime_slot_for<true, true>(runtime, scratch, header, message);
                    return;
            }
        }

        struct configured_drain_context {
            runtime_state& runtime;
            consumer_scratch& scratch;
        };

        static void consume_configured_record(
                void* context, const backend::record_slot_header& header, std::string_view message) {
            auto& drain = *static_cast<configured_drain_context*>(context);
            emit_runtime_slot(drain.runtime, drain.scratch, header, message);
        }

        static bool drain_runtime_queues(runtime_state& runtime, consumer_scratch& scratch) {
            auto context = configured_drain_context{runtime, scratch};
            return runtime.producer_backend->drain_ready(&context, &consume_configured_record);
        }

        static void drain_runtime_queues_until_idle(runtime_state& runtime, consumer_scratch& scratch) {
            auto context = configured_drain_context{runtime, scratch};
            runtime.producer_backend->drain_until_idle(&context, &consume_configured_record);
        }

        static void drain_flush_targets(runtime_state& runtime, consumer_scratch& scratch) {
            auto context = configured_drain_context{runtime, scratch};
            runtime.producer_backend->drain_flush(&context, &consume_configured_record);
        }

        static void flush_runtime_sinks(runtime_state& runtime) {
            for (const auto& sink : runtime.configured_sinks) {
                sink.sink->flush();
            }
            for (const auto& sink : runtime.custom_sinks) {
                sink.sink->flush();
            }
        }

        static void consumer_main(runtime_state& runtime) {
            auto scratch = consumer_scratch{};

            for (;;) {
                // Sample the wake ticket before any condition check: every waker mutates its
                // condition first, then bumps wake_seq — so a wake landing after this load
                // makes the wait below return immediately instead of sleeping through it.
                auto seq = runtime.wake_seq.load(std::memory_order_acquire);

                auto all_drained = drain_runtime_queues(runtime, scratch);

                auto requested = runtime.flush_requested.load(std::memory_order_acquire);
                if (requested != runtime.flush_completed.load(std::memory_order_acquire)) {
                    drain_flush_targets(runtime, scratch);
                    flush_runtime_sinks(runtime);
                    runtime.flush_completed.store(requested, std::memory_order_release);
                    runtime.flush_completed.notify_all();
                    continue;
                }

                if (runtime.stop_requested.load(std::memory_order_acquire)) {
                    break;
                }

                if (!all_drained) {
                    continue;
                }

                runtime.wake_seq.wait(seq, std::memory_order_acquire);
            }

            drain_runtime_queues_until_idle(runtime, scratch);
            flush_runtime_sinks(runtime);
        }

        static void ensure_consumer_started_locked(runtime_state& runtime) {
            if (runtime.consumer_started) {
                return;
            }

            runtime.stop_requested.store(false, std::memory_order_relaxed);
            // std::thread stores args by value after decay, so use std::ref
            runtime.consumer_thread = std::thread{consumer_main, std::ref(runtime)};
            runtime.consumer_started = true;
        }

        static channel_handle make_channel_route_locked(
                runtime_state& st,
                backend::runtime_producer_backend& producer_backend,
                channel_registration registration) {
            auto channel_name = std::string{registration.name};
            if (st.channel_ids.contains(channel_name)) {
                throw std::invalid_argument{"A channel with the name {} already exists"_format(registration.name)};
            }

            if (st.channels.size() >= static_cast<size_t>(invalid_channel_id)) {
                throw std::length_error{"channel registry exhausted"};
            }

            auto sink = configured_sink_for_locked(st, registration);

            auto id = static_cast<channel_id>(st.channels.size());
            st.channels.emplace_back(id, std::move(channel_name));

            auto& state = st.channels.back();
            state.configure_level(st.global_level.load(std::memory_order_relaxed));
            state.sink = backend::sink_entry{
                    .sink = std::move(sink),
                    .pattern = registration.pattern,
                    .render_buffer = {},
                    .color = registration.color,
                    .requirements = registration.time_requirements,
            };
            st.channel_ids.emplace(state.name, id);

            producer_backend.register_channel(registration.runtime_mode, registration.huge_pages);

            return channel_handle{&state};
        }

        log_level get_global_level(backend::runtime_producer_backend& producer_backend) {
            if (auto* st = try_access_state()) {
                if (st->producer_backend != &producer_backend) {
                    throw std::invalid_argument{"level query uses a different compile-time global configuration"};
                }
                return st->global_level.load(std::memory_order_relaxed);
            }

            return log_level::info;
        }

        void set_global_level(backend::runtime_producer_backend& producer_backend, log_level level) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);
            if (st.producer_backend != &producer_backend) {
                throw std::invalid_argument{"level update uses a different compile-time global configuration"};
            }

            st.global_level.store(level, std::memory_order_relaxed);

            auto running = control.lifecycle.load(std::memory_order_relaxed) == runtime_lifecycle::running;
            for (auto& route : st.channels) {
                if (running) {
                    route.set_running_level(level);
                }
                else {
                    route.configure_level(level);
                }
            }
        }

        void set_route_level(
                backend::runtime_producer_backend& producer_backend, route_state& route, log_level level) noexcept {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto* st = control.state.load(std::memory_order_acquire);
            if (!st || st->producer_backend != &producer_backend) {
                return;
            }

            if (control.lifecycle.load(std::memory_order_relaxed) == runtime_lifecycle::running) {
                route.set_running_level(level);
            }
            else {
                route.configure_level(level);
            }
        }

        channel_handle make_channel_route(
                backend::runtime_producer_backend& producer_backend, channel_registration registration) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);
            if (control.lifecycle.load(std::memory_order_acquire) != runtime_lifecycle::configuring) {
                throw std::invalid_argument{"runtime has started; cannot register new channels"};
            }
            if (st.producer_backend != &producer_backend) {
                throw std::invalid_argument{"channel uses a different compile-time global configuration"};
            }
            return make_channel_route_locked(st, producer_backend, std::move(registration));
        }

        void add_sink_route(backend::runtime_producer_backend& producer_backend, backend::sink_entry entry) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);

            if (control.lifecycle.load(std::memory_order_acquire) != runtime_lifecycle::configuring) {
                throw std::invalid_argument{"runtime has started; cannot add sinks"};
            }
            if (st.producer_backend != &producer_backend) {
                throw std::invalid_argument{"sink uses a different compile-time global configuration"};
            }
            if (!entry.sink) {
                throw std::invalid_argument{"add_sink requires a non-null sink"};
            }

            st.custom_sink_requirements |= entry.requirements;
            st.custom_sinks.push_back(std::move(entry));
        }

        void start_backend(backend::runtime_producer_backend& producer_backend) {
            auto& control = access_control();
            std::lock_guard lock{control.mutex};
            auto& st = ensure_state_created_locked(control);
            register_process_exit_hook_locked(control);

            auto lifecycle = control.lifecycle.load(std::memory_order_acquire);
            if (lifecycle == runtime_lifecycle::running) {
                return;
            }
            if (lifecycle != runtime_lifecycle::configuring) {
                throw std::invalid_argument{"runtime cannot be started in its current state"};
            }
            if (st.channels.empty()) {
                throw std::invalid_argument{"runtime requires at least one channel before start"};
            }
            if (st.producer_backend != &producer_backend) {
                throw std::invalid_argument{"start uses a different compile-time global configuration"};
            }

            producer_backend.prepare_start();

            for (auto& route : st.channels) {
                route.activate();
            }

            ensure_consumer_started_locked(st);
            control.lifecycle.store(runtime_lifecycle::running, std::memory_order_release);
        }

        void flush_backend(backend::runtime_producer_backend& producer_backend) {
            auto& control = access_control();
            auto* st = control.state.load(std::memory_order_acquire);
            if (!st || st->producer_backend != &producer_backend) {
                return;
            }

            st->active_flushers.fetch_add(1, std::memory_order_acq_rel);
            if (control.lifecycle.load(std::memory_order_acquire) != runtime_lifecycle::running) {
                if (st->active_flushers.fetch_sub(1, std::memory_order_release) == 1) {
                    st->active_flushers.notify_all();
                }
                return;
            }

            if (st->consumer_thread.get_id() == std::this_thread::get_id()) {
                if (st->active_flushers.fetch_sub(1, std::memory_order_release) == 1) {
                    st->active_flushers.notify_all();
                }
                throw std::logic_error{"flush cannot be called from the consumer thread"};
            }
            auto target = st->flush_requested.fetch_add(1, std::memory_order_relaxed) + 1u;
            wake_consumer(*st);

            for (auto completed = st->flush_completed.load(std::memory_order_acquire); completed < target;
                 completed = st->flush_completed.load(std::memory_order_acquire)) {
                st->flush_completed.wait(completed, std::memory_order_acquire);
            }
            if (st->active_flushers.fetch_sub(1, std::memory_order_release) == 1) {
                st->active_flushers.notify_all();
            }
        }

#if UNLOG_DIAGNOSTIC
        backend::producer_stats backend_stats(backend::runtime_producer_backend& producer_backend) {
            return producer_backend.stats();
        }
#endif

        void prewarm_backend(backend::runtime_producer_backend& producer_backend) {
            auto& control = access_control();
            if (control.lifecycle.load(std::memory_order_acquire) != runtime_lifecycle::running) {
                return;
            }
            if (auto* st = control.state.load(std::memory_order_acquire);
                st && st->producer_backend == &producer_backend) {
                producer_backend.prewarm_thread();
            }
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

            if (!st->consumer_started) {
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
            if (!st || !st->producer_backend) {
                return 0;
            }

            return st->producer_backend->producer_count();
        }
    }  // namespace test

}  // namespace un::log
