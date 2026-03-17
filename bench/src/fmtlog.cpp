#define FMTLOG_BLOCK 0
#define FMTLOG_QUEUE_SIZE 4194304
#include "provider.hpp"

#include <fmtlog.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace {

    struct fmtlog_provider {
        static constexpr std::string_view name() noexcept { return "fmtlog"; }

        static void initialize(const unlog_bench::benchmark_options& options) {
            std::call_once(init_flag(), [&options]() {
                set_sink(options);
                fmtlog::setHeaderPattern("{m}");
                fmtlog::startPollingThread(100);
                FMTLOG(fmtlog::INF, "warmup");
                fmtlog::stopPollingThread();
            });
        }

        static void prepare_thread() { fmtlog::preallocate(); }

        static void prepare_benchmark() { ensure_polling_thread_running(); }

        static void log(const unlog_bench::log_message& message) {
            FMTLOG(fmtlog::INF,
                   "u64: {}, i64: {}, u32: {}, i32: {}, s: {}",
                   static_cast<unsigned long long>(message.u64),
                   static_cast<long long>(message.i64),
                   static_cast<unsigned long>(message.u32),
                   static_cast<long>(message.i32),
                   message.text.data());
        }

        static void reset_state() {
            fmtlog::stopPollingThread();
            polling_thread_running() = false;
        }

      private:
        static std::once_flag& init_flag() {
            static std::once_flag flag;
            return flag;
        }

        static bool& polling_thread_running() {
            static bool running = false;
            return running;
        }

        static void ensure_polling_thread_running() {
            if (polling_thread_running()) {
                return;
            }

            fmtlog::startPollingThread(100);
            polling_thread_running() = true;
        }

        static void set_sink(const unlog_bench::benchmark_options& options) {
            switch (options.sink) {
                case unlog_bench::sink_kind::file:
                    std::filesystem::create_directories(unlog_bench::ensure_output_dir());
                    fmtlog::setLogFile(unlog_bench::file_sink_path(name()).string().c_str(), true);
                    return;
                case unlog_bench::sink_kind::stdout:
                    fmtlog::setLogFile(stdout);
                    return;
                case unlog_bench::sink_kind::fd:
                    fmtlog::setLogFile(unlog_bench::null_sink_path().string().c_str(), true);
                    return;
            }

            throw std::invalid_argument{"invalid sink kind"};
        }
    };

}  // namespace

int main(int argc, char** argv) {
    return unlog_bench::run_benchmarks<fmtlog_provider>(argc, argv);
}
