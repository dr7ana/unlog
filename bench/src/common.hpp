#pragma once

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace unlog_bench {

    enum class sink_kind {
        file,
        stdout,
        fd,
    };

    struct log_message {
        std::uint64_t u64;
        std::uint32_t u32;
        std::int64_t i64;
        std::int32_t i32;
        std::array<char, 128> text;
    };

    struct benchmark_options {
        sink_kind sink{sink_kind::file};
        std::filesystem::path output_dir{"bench-results"};
        std::size_t dataset_size{8192};
        std::size_t thread_bufsize{1u << 22};
    };

    inline constexpr std::array<int, 5> thread_counts{
            1,
            2,
            4,
            8,
            16,
    };

    inline constexpr int repeated_iterations{2000};
    inline constexpr int repeated_repetitions{20};
    inline constexpr double min_time_seconds{3.0};

    const benchmark_options& options() noexcept;
    void parse_options(int* argc, char** argv);
    inline constexpr std::string_view sink_name(sink_kind sink) noexcept {
        if (sink == sink_kind::stdout) {
            return "stdout";
        }

        if (sink == sink_kind::fd) {
            return "fd";
        }

        return "file";
    }

    std::string benchmark_name(std::string_view provider_name, std::string_view phase);
    std::vector<log_message>& dataset();
    std::filesystem::path ensure_output_dir();
    std::filesystem::path file_sink_path(std::string_view provider_name);
    std::filesystem::path null_sink_path();

}  // namespace unlog_bench
