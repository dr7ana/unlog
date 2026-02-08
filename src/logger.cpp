#include "unlog.hpp"

#include "unlog/backend/producer.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

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
            if (type == ClockType::system)
                return &clock_now_ns_for<ClockType::system>;

            return &clock_now_ns_for<ClockType::steady>;
        }

        template <typename Backend>
        struct runtime_lane {
            alignas(Backend) std::byte storage[sizeof(Backend)]{};
            bool constructed{false};
            ClockType clock_type{ClockType::steady};
            bool clock_type_set{false};
            clock_now_fn_t clock_now_fn{clock_now_fn_for<ClockType::steady>()};

            constexpr auto backend_ptr(this auto& self) noexcept {
                using self_t = std::remove_reference_t<decltype(self)>;
                using ptr_t = std::conditional_t<std::is_const_v<self_t>, const Backend*, Backend*>;
                return std::launder(reinterpret_cast<ptr_t>(self.storage));
            }
        };

        using sqpoll_runtime_lane = runtime_lane<backend::sqpoll_backend>;

        struct runtime_state {
            std::unordered_map<std::string, logger_ptr> loggers;
            logger_ptr default_logger;
            sqpoll_runtime_lane sqpoll_lane;
            log_level default_level{log_level::info};
            // TODO(multithread-hardening): if setup/logging can run concurrently at high thread counts,
            // revisit this gate with explicit acquire/release ordering and stronger transition semantics.
            std::atomic<bool> is_active{false};
        };

        runtime_state& access_state() {
            static runtime_state instance;
            return instance;
        }

        static constexpr auto& sqpoll_lane(runtime_state& st) {
            return st.sqpoll_lane;
        }

        void construct_lane(sqpoll_runtime_lane& lane) {
            if (!lane.constructed) {
                std::construct_at(lane.backend_ptr());
                lane.constructed = true;
            }
        }

        void destroy_lane(runtime_state& st) {
            auto& lane = st.sqpoll_lane;
            if (lane.constructed) {
                std::destroy_at(lane.backend_ptr());
                lane.constructed = false;
            }

            lane.clock_type = ClockType::steady;
            lane.clock_type_set = false;
            lane.clock_now_fn = clock_now_fn_for<ClockType::steady>();
        }

        void set_lane_clock_type(sqpoll_runtime_lane& lane, ClockType type) {
            if (!lane.clock_type_set) {
                lane.clock_type = type;
                lane.clock_type_set = true;
                lane.clock_now_fn = clock_now_fn_for(type);
                return;
            }

            if (lane.clock_type != type)
                throw std::invalid_argument{"cannot mix different backend clock types in one lane"};
        }

        auto& ensure_sqpoll_lane(runtime_state& st, ClockType timestamp_mode) {
            auto& lane = sqpoll_lane(st);
            construct_lane(lane);
            set_lane_clock_type(lane, timestamp_mode);
            return lane;
        }

        std::optional<std::string_view> read_payload(const backend::record_view& view) {
            if (!view.has_header())
                return std::nullopt;

            auto payload_size = view.header->payload_size;
            if (payload_size > view.payload.size())
                return std::nullopt;

            auto* message_ptr = reinterpret_cast<const char*>(view.payload.data());
            return std::string_view{message_ptr, payload_size};
        }

        static std::optional<backend::log_entry> make_log_entry(const backend::record_view& view) {
            auto message = read_payload(view);
            if (!message.has_value())
                return std::nullopt;

            return backend::log_entry{
                    .logger_name = view.header->logger_name ? view.header->logger_name : "",
                    .level = view.level(),
                    .source_location =
                            source_loc{
                                    .filename = view.header->source_file,
                                    .line = view.header->source_line,
                                    .function = view.header->source_function,
                            },
                    .message = std::string{message->data(), message->size()},
                    .timestamp = view.header->timestamp,
            };
        }

        static void emit_record_to_backend(backend::sqpoll_backend* active_backend, const backend::record_view& view) {
            if (!active_backend || !view.has_header())
                return;

            auto rec = make_log_entry(view);
            if (!rec.has_value())
                return;

            active_backend->log(std::move(*rec));
        }

        void mark_runtime_active_after_commit(uint64_t sequence) noexcept {
            if (sequence == 0)
                access_state().is_active.store(true, std::memory_order_relaxed);
        }

        static constexpr auto truncate_marker = "[truncated]"sv;

        void log_message(
                backend::sqpoll_backend* active_backend,
                clock_now_fn_t clock_now_fn,
                const char* logger_name,
                size_t max_message_size,
                bool overflow_drop,
                bool can_truncate,
                const source_loc& source_location,
                log_level level,
                std::string&& rendered) {

            if (!active_backend || !clock_now_fn)
                return;

            auto& producer = backend::get_thread_producer();

            if (rendered.size() > max_message_size) {
                if (overflow_drop || !can_truncate) {
                    producer.count_dropped();
                    return;
                }

                rendered.resize(max_message_size);
                if (max_message_size <= truncate_marker.size()) {
                    if (max_message_size > 0)
                        std::memcpy(rendered.data(), truncate_marker.data(), max_message_size);
                }
                else {
                    auto marker_offset = max_message_size - truncate_marker.size();
                    std::memcpy(rendered.data() + marker_offset, truncate_marker.data(), truncate_marker.size());
                }

                producer.count_truncated();
            }

            auto sequence = producer.next_sequence();
            auto timestamp = clock_now_fn();
            auto rec = backend::log_entry{
                    .logger_name = std::string{logger_name ? logger_name : ""},
                    .level = level,
                    .source_location =
                            source_loc{
                                    .filename = source_location.filename,
                                    .line = source_location.line,
                                    .function = source_location.function,
                            },
                    .message = std::move(rendered),
                    .timestamp = timestamp,
            };

            active_backend->log(std::move(rec));
            mark_runtime_active_after_commit(sequence);
            producer.count_emitted();
        }

        template <detail::basic_config_type Conf>
        static logger_ptr make_and_register_logger_locked(runtime_state& st, const Conf& conf) {
            auto& lane = ensure_sqpoll_lane(st, detail::config_clock_type_v<Conf>);
            auto* active_backend = lane.backend_ptr();
            active_backend->init(conf);

            auto created = std::make_shared<logger>(conf);
            created->bind_backend(active_backend, lane.clock_now_fn);
            created->set_level(st.default_level);
            st.loggers.emplace(conf.name, created);
            return created;
        }

        void ensure_default_logger(runtime_state& st) {
            if (st.default_logger)
                return;

            auto conf = config<>::make_sqpoll();
            st.default_logger = make_and_register_logger_locked(st, conf);
        }

        log_level get_default_level() {
            auto& st = access_state();

            return st.default_level;
        }

        void set_default_level(log_level level) {
            auto& st = access_state();

            st.default_level = level;

            for (auto& [_, lg] : st.loggers)
                lg->set_level(level);
        }

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
                std::optional<fs::path> unix_dgram_path) {

            auto& st = access_state();

            if (st.is_active.load(std::memory_order_relaxed))
                throw std::invalid_argument{"runtime is active; cannot register new loggers"};

            auto logger_name_str = std::string{logger_name};
            if (st.loggers.contains(logger_name_str))
                throw std::invalid_argument{"A logger with the name {} already exists"_format(logger_name)};

            auto& lane = ensure_sqpoll_lane(st, timestamp_mode);
            auto* active_backend = lane.backend_ptr();
            active_backend->init(
                    sink_type,
                    format,
                    std::move(filename),
                    timestamp_mode,
                    strict_nonblocking,
                    sqpoll_queue_depth,
                    output_fd,
                    std::move(unix_dgram_path));

            created->bind_backend(active_backend, lane.clock_now_fn);
            created->set_level(st.default_level);
            st.loggers.emplace(std::move(logger_name_str), created);

            if (make_default || !st.default_logger)
                st.default_logger = std::move(created);
        }

        void add_sink_route(sink_ptr sink, ClockType timestamp_mode, std::string_view format) {
            auto& st = access_state();

            if (st.is_active.load(std::memory_order_relaxed))
                throw std::invalid_argument{"runtime is active; cannot add sinks"};

            auto* active_backend = ensure_sqpoll_lane(st, timestamp_mode).backend_ptr();
            active_backend->add_sink(std::move(sink), std::optional<std::string>{format});
        }

        void flush_backend() {
            auto& st = access_state();
            if (!st.sqpoll_lane.constructed) {
                return;
            }

            auto* active_backend = st.sqpoll_lane.backend_ptr();
            backend::drain_batch(
                    std::numeric_limits<size_t>::max(),
                    [active_backend](backend::producer&, const backend::record_view& view) {
                        emit_record_to_backend(active_backend, view);
                        return true;
                    });
            active_backend->flush();
        }

        backend::producer_stats backend_stats() {
            auto out = backend::producer_stats{};
            auto producers = backend::producer_snapshot();
            for (auto& p : producers) {
                auto snapshot = p->stats();
                out.emitted += snapshot.emitted;
                out.dropped += snapshot.dropped;
                out.truncated += snapshot.truncated;
            }
            return out;
        }

    }  // namespace detail

    namespace test {
        void get_runtime_backend(const std::function<void()>& fn) {
            if (!fn)
                return;

            auto& st = detail::access_state();

            if (!st.sqpoll_lane.constructed) {
                return;
            }

            fn();
        }

        void get_runtime_sqpoll_backend(const std::function<void(backend::sqpoll_backend&)>& fn) {
            if (!fn)
                return;

            auto& st = detail::access_state();

            if (!st.sqpoll_lane.constructed) {
                return;
            }

            fn(*st.sqpoll_lane.backend_ptr());
        }

        void reset_runtime_for_test() {
            auto& st = detail::access_state();

            st.loggers.clear();
            st.default_logger.reset();
            detail::destroy_lane(st);
            st.default_level = log_level::info;
            st.is_active.store(false, std::memory_order_relaxed);
        }
    }  // namespace test

    const logger_ptr& global_logger() {
        auto& st = detail::access_state();
        detail::ensure_default_logger(st);
        return st.default_logger;
    }

    void logger::set_level(log_level level) {
        level_.store(level, std::memory_order_relaxed);
    }

    log_level logger::level() const {
        return level_.load(std::memory_order_relaxed);
    }

    void flush() {
        detail::flush_backend();
    }

}  // namespace un::log
