#include "unlog.hpp"

#include "provider.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <utility>

namespace {

    struct unlog_provider {
        static constexpr auto global = unlog::global_config{
                .thread_bufsize = unlog_bench::thread_bufsize_bytes,
                .huge_thread_bufsize = unlog_bench::thread_bufsize_bytes,
        };
        using logger = unlog::configured<global>;
        using config_type =
                unlog::config<unlog::options::threadsafe, unlog::options::huge_pages, unlog::options::pattern<"%v">>;
        using channel_type = unlog::channel<global, unlog::detail::channel_policy_for<config_type>>;

        static constexpr std::string_view name() noexcept { return "unlog"; }

        static void initialize(const unlog_bench::benchmark_options& options) {
            std::call_once(init_flag(), [&options]() {
                std::ignore = channel(options);
                logger::set_global_level(unlog::log_level::info);
                logger::start();
                unlog::info(channel(), "warmup");
                logger::flush();
            });
        }

        static void prepare_thread() { logger::prewarm_thread(); }

        static void log(const unlog_bench::log_message& message) {
            unlog::info(
                    channel(),
                    "u64: {}, i64: {}, u32: {}, i32: {}, s: {}",
                    static_cast<unsigned long long>(message.u64),
                    static_cast<long long>(message.i64),
                    static_cast<unsigned long>(message.u32),
                    static_cast<long>(message.i32),
                    message.text.data());
        }

        static void reset_state() { logger::flush(); }

      private:
        static std::once_flag& init_flag() {
            static std::once_flag flag;
            return flag;
        }

        static channel_type& channel(const unlog_bench::benchmark_options& options = unlog_bench::options()) {
            static channel_type instance = logger::make_channel(make_config(options));
            return instance;
        }

        static int& sink_fd() {
            static int fd = -1;
            return fd;
        }

        static config_type make_config(const unlog_bench::benchmark_options& options) {
            switch (options.sink) {
                case unlog_bench::sink_kind::file:
                    std::filesystem::create_directories(unlog_bench::ensure_output_dir());
                    return config_type::make_file(unlog_bench::file_sink_path(name()).string(), "root");
                case unlog_bench::sink_kind::stdout:
                    return config_type::make("root");
                case unlog_bench::sink_kind::fd:
                    sink_fd() = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
                    if (sink_fd() < 0) {
                        throw std::runtime_error{"failed to open /dev/null"};
                    }

                    return config_type::make_fd(sink_fd(), "root");
            }

            throw std::invalid_argument{"invalid sink kind"};
        }
    };

}  // namespace

int main(int argc, char** argv) {
    return unlog_bench::run_benchmarks<unlog_provider>(argc, argv);
}
