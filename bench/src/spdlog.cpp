#include "provider.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>

namespace {

    struct spdlog_provider {
        static constexpr std::string_view name() noexcept { return "spdlog"; }

        static void initialize(const unlog_bench::benchmark_options& options) {
            std::call_once(init_flag(), [&options]() {
                spdlog::init_thread_pool(8 * 1024, 1);
                auto logger = make_logger(options);
                logger->set_level(spdlog::level::info);
                logger->set_pattern("%v");
                spdlog::set_default_logger(std::move(logger));
                SPDLOG_INFO("warmup");
                spdlog::default_logger()->flush();
            });
        }

        static void prepare_thread() {}

        static void log(const unlog_bench::log_message& message) {
            SPDLOG_INFO(
                    "u64: {}, i64: {}, u32: {}, i32: {}, s: {}",
                    static_cast<unsigned long long>(message.u64),
                    static_cast<long long>(message.i64),
                    static_cast<unsigned long>(message.u32),
                    static_cast<long>(message.i32),
                    message.text.data());
        }

        static void reset_state() {
            if (spdlog::default_logger()) {
                spdlog::default_logger()->flush();
            }
        }

      private:
        static std::once_flag& init_flag() {
            static std::once_flag flag;
            return flag;
        }

        static std::shared_ptr<spdlog::logger> make_logger(const unlog_bench::benchmark_options& options) {
            auto thread_pool = spdlog::thread_pool();
            if (!thread_pool) {
                throw std::runtime_error{"spdlog thread pool is not initialized"};
            }

            switch (options.sink) {
                case unlog_bench::sink_kind::file:
                    std::filesystem::create_directories(unlog_bench::ensure_output_dir());
                    return spdlog::basic_logger_mt<spdlog::async_factory_impl<spdlog::async_overflow_policy::discard_new>>(
                            "root", unlog_bench::file_sink_path(name()).string(), true);
                case unlog_bench::sink_kind::stdout:
                    return std::make_shared<spdlog::async_logger>(
                            "root",
                            std::make_shared<spdlog::sinks::stdout_sink_mt>(),
                            thread_pool,
                            spdlog::async_overflow_policy::discard_new);
                case unlog_bench::sink_kind::fd:
                    return spdlog::basic_logger_mt<spdlog::async_factory_impl<spdlog::async_overflow_policy::discard_new>>(
                            "root", unlog_bench::null_sink_path().string(), true);
            }

            throw std::invalid_argument{"invalid sink kind"};
        }
    };

}  // namespace

int main(int argc, char** argv) {
    return unlog_bench::run_benchmarks<spdlog_provider>(argc, argv);
}
