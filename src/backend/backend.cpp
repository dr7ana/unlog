#include "unlog/backend/backend.hpp"

#include "internal.hpp"

namespace un::log::backend {

    auto startup_steady_time = std::chrono::steady_clock::now();
    auto startup_system_time = std::chrono::system_clock::now();
    auto startup_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(startup_steady_time.time_since_epoch());
    auto startup_system_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(startup_system_time.time_since_epoch());

    static constexpr time_context resolve_time_context(const log_entry& rec, ClockType clock_type) {
        auto sample_system = std::chrono::system_clock::time_point{};
        auto elapsed = std::chrono::nanoseconds{0};

        if (clock_type == ClockType::system) {
            auto system_ticks = ticks_to_ns(rec.timestamp);
            sample_system = std::chrono::system_clock::time_point{system_ticks};
            elapsed = system_ticks - startup_system_ns;
        }
        else {
            auto steady_ticks = ticks_to_ns(rec.timestamp);
            elapsed = steady_ticks - startup_steady_ns;
            sample_system = startup_system_time + elapsed;
        }

        return time_context{
                .tm = local_time(sample_system),
                .millis = millis_part(sample_system),
                .elapsed = format_elapsed(elapsed),
        };
    }

    sqpoll_backend::~sqpoll_backend() {
        std::unique_lock lock{mutex_};
        sqpoll_runtime_stop(runtime_);
    }

    void sqpoll_backend::init(
            SinkType sink_type,
            std::string_view format,
            std::optional<fs::path> filename,
            ClockType timestamp_mode,
            bool strict_nonblocking,
            size_t sqpoll_queue_depth,
            std::optional<int> output_fd,
            std::optional<fs::path> unix_dgram_path) {
        std::unique_lock lock{mutex_};
        clock_type_.store(timestamp_mode, std::memory_order_relaxed);
        sqpoll_runtime_start(runtime_, static_cast<uint32_t>(sqpoll_queue_depth));
        sqpoll_runtime_add_endpoint(
                runtime_,
                sink_type,
                format,
                std::move(filename),
                strict_nonblocking,
                output_fd,
                std::move(unix_dgram_path),
                std::nullopt);
    }

    void sqpoll_backend::add_sink(sink_ptr sink_obj, std::optional<std::string> pattern) {
        if (!sink_obj)
            throw std::invalid_argument{"add_sink requires a non-null sink"};

        auto use_color_pattern = sink_obj->supports_color();
        std::unique_lock lock{mutex_};
        custom_sinks_.push_back(
                sink_entry{
                        .sink = std::move(sink_obj),
                        .pattern = pattern.value_or(use_color_pattern ? DEFAULT_PATTERN_COLOR : DEFAULT_PATTERN),
                });
    }

    void sqpoll_backend::log(log_entry&& rec) {
        assert(rec.timestamp != 0 && "sqpoll_backend requires pre-stamped log_entry timestamp");
        auto clock_type = clock_type_.load(std::memory_order_relaxed);

        auto time = resolve_time_context(rec, clock_type);
        std::vector<line_cache_entry> line_cache;
        uint64_t emitted_count = 0;
        uint64_t dropped_count = 0;
        auto has_any_target = false;
        std::vector<sink_entry> sink_snapshot{};

        {
            std::unique_lock lock{mutex_};
            dropped_count += sqpoll_runtime_take_completion_failures(runtime_);
            has_any_target = !runtime_.endpoints.empty() || !custom_sinks_.empty();
            sink_snapshot = custom_sinks_;
            line_cache.reserve(runtime_.endpoints.size() + sink_snapshot.size());

            for (auto& endpoint : runtime_.endpoints) {
                auto line =
                        format_cache_line(line_cache, endpoint.pattern, true, rec, time.tm, time.millis, time.elapsed);
                if (sqpoll_runtime_write(runtime_, endpoint, line))
                    ++emitted_count;
                else
                    ++dropped_count;
            }

            dropped_count += sqpoll_runtime_take_completion_failures(runtime_);
        }

        for (auto& sink : sink_snapshot) {
            auto line = format_cache_line(line_cache, sink.pattern, false, rec, time.tm, time.millis, time.elapsed);
            sink.sink->write(line);
            sink.sink->write("\n"sv);
            ++emitted_count;
        }

        if (!has_any_target)
            ++dropped_count;

        emitted_.fetch_add(emitted_count, std::memory_order_relaxed);
        dropped_.fetch_add(dropped_count, std::memory_order_relaxed);
    }

    void sqpoll_backend::flush() {
        std::vector<sink_entry> sink_snapshot{};
        uint64_t dropped_count = 0;
        {
            std::unique_lock lock{mutex_};
            sqpoll_runtime_flush(runtime_);
            dropped_count += sqpoll_runtime_take_completion_failures(runtime_);
            sink_snapshot = custom_sinks_;
        }

        for (auto& sink : sink_snapshot)
            sink.sink->flush();

        dropped_.fetch_add(dropped_count, std::memory_order_relaxed);
    }

    producer_stats sqpoll_backend::stats_snapshot() const {
        return producer_stats{
                .emitted = emitted_.load(std::memory_order_relaxed),
                .dropped = dropped_.load(std::memory_order_relaxed),
                .truncated = truncated_.load(std::memory_order_relaxed),
        };
    }

}  // namespace un::log::backend
