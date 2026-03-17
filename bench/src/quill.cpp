#include "provider.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <filesystem>
#include <mutex>

namespace {

    struct frontend_options {
        static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
        static constexpr std::size_t initial_queue_capacity = 4ull * 1024ull * 1024ull;
        static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
        static constexpr std::size_t unbounded_queue_max_capacity = 2ull * 1024ull * 1024ull * 1024ull;
        static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
    };

    using frontend = quill::FrontendImpl<frontend_options>;
    using logger_type = quill::LoggerImpl<frontend_options>;

    struct quill_provider {
        static constexpr std::string_view name() noexcept { return "quill"; }

        static void initialize(const unlog_bench::benchmark_options& options) {
            std::call_once(init_flag(), [&options]() {
                quill::BackendOptions backend_options;
                quill::Backend::start(backend_options);
                auto sink = make_sink(options);
                logger() = frontend::create_or_get_logger(
                        "root", std::move(sink), quill::PatternFormatterOptions("%(message)", "%H:%M:%S.%Qms"));
                logger()->set_log_level(quill::LogLevel::Info);
                LOG_INFO(logger(), "warmup");
                logger()->flush_log();
            });
        }

        static void prepare_thread() { frontend::preallocate(); }

        static void log(const unlog_bench::log_message& message) {
            LOG_INFO(
                    logger(),
                    "u64: {}, i64: {}, u32: {}, i32: {}, s: {}",
                    static_cast<unsigned long long>(message.u64),
                    static_cast<long long>(message.i64),
                    static_cast<unsigned long>(message.u32),
                    static_cast<long>(message.i32),
                    message.text.data());
        }

        static void reset_state() { logger()->flush_log(); }

      private:
        static std::once_flag& init_flag() {
            static std::once_flag flag;
            return flag;
        }

        static logger_type*& logger() {
            static logger_type* instance = nullptr;
            return instance;
        }

        static std::shared_ptr<quill::Sink> make_sink(const unlog_bench::benchmark_options& options) {
            switch (options.sink) {
                case unlog_bench::sink_kind::file:
                    std::filesystem::create_directories(unlog_bench::ensure_output_dir());
                    return frontend::create_or_get_sink<quill::FileSink>(unlog_bench::file_sink_path(name()).string());
                case unlog_bench::sink_kind::stdout:
                {
                    quill::ConsoleSinkConfig sink_config;
                    sink_config.set_colour_mode(quill::ConsoleSinkConfig::ColourMode::Never);
                    sink_config.set_stream("stdout");
                    return frontend::create_or_get_sink<quill::ConsoleSink>("stdout", sink_config);
                }
                case unlog_bench::sink_kind::fd:
                    return frontend::create_or_get_sink<quill::FileSink>(unlog_bench::null_sink_path().string());
            }

            throw std::invalid_argument{"invalid sink kind"};
        }
    };

}  // namespace

int main(int argc, char** argv) {
    return unlog_bench::run_benchmarks<quill_provider>(argc, argv);
}
