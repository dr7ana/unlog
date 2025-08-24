#include "unlog.hpp"
#include "unlog/logger.hpp"

namespace un::log {
    //
    const auto started_at = std::chrono::steady_clock::now();

    // Custom log formatting flag that prints the elapsed time since startup
    class startup_elapsed_flag : public spdlog::custom_flag_formatter {
      private:
        static constexpr fmt::format_string<
                std::chrono::hours::rep,
                std::chrono::minutes::rep,
                std::chrono::seconds::rep,
                std::chrono::milliseconds::rep>
                format_hours{"+{0:d}h{1:02d}m{2:02d}.{3:03d}s"},  // >= 1h
                format_minutes{"+{1:d}m{2:02d}.{3:03d}s"},        // >= 1min
                format_seconds{"+{2:d}.{3:03d}s"};                // < 1min

      public:
        void format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest) override {
            using namespace std::literals;
            auto elapsed = std::chrono::steady_clock::now() - started_at;

            dest.append(fmt::format(
                    elapsed >= 1h     ? format_hours
                    : elapsed >= 1min ? format_minutes
                                      : format_seconds,
                    std::chrono::duration_cast<std::chrono::hours>(elapsed).count(),
                    (std::chrono::duration_cast<std::chrono::minutes>(elapsed) % 1h).count(),
                    (std::chrono::duration_cast<std::chrono::seconds>(elapsed) % 1min).count(),
                    (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed) % 1s).count()));
        }

        std::unique_ptr<custom_flag_formatter> clone() const override {
            return std::make_unique<startup_elapsed_flag>();
        }
    };

    template <typename T, typename U>
    bool is_instance(const U* ptr) {
        return dynamic_cast<const T*>(ptr) != nullptr;
    }

    bool is_color_sink(const spdlog::sink_ptr& sink) {
        auto* s = sink.get();
        return is_instance<spdlog::sinks::ansicolor_stdout_sink_mt>(s) ||
               is_instance<spdlog::sinks::ansicolor_stderr_sink_mt>(s);
    }

    void set_sink_format(const spdlog::sink_ptr& sink, std::optional<std::string> pattern = std::nullopt) {
        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<startup_elapsed_flag>('*');
        if (pattern)
            formatter->set_pattern(*std::move(pattern));
        else if (is_color_sink(sink))
            formatter->set_pattern(DEFAULT_PATTERN_COLOR);
        else
            formatter->set_pattern(DEFAULT_PATTERN);
        sink->set_formatter(std::move(formatter));
    }

    namespace detail {
        void add_sink(sink_ptr sink) {
            set_sink_format(sink);
            master_sink->add_sink(std::move(sink));
        }

        void add_sink(const Config& conf, sink_ptr sink) {
            set_sink_format(sink, conf.format);
            master_sink->set_sinks({std::move(sink)});
        }
    }  // namespace detail

}  // namespace un::log
