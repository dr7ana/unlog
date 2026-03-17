#include "common.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace unlog_bench {
    namespace {

        benchmark_options g_options{};

        std::uint64_t next_random(std::uint64_t& state) noexcept {
            state += 0x9e3779b97f4a7c15ULL;
            std::uint64_t value = state;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31U);
        }

        constexpr bool starts_with(std::string_view value, std::string_view prefix) noexcept {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        std::size_t parse_size(std::string_view value, std::string_view name) {
            std::size_t parsed{};
            const char* begin = value.data();
            const char* end = value.data() + value.size();
            auto result = std::from_chars(begin, end, parsed);
            if (result.ec != std::errc{} || result.ptr != end) {
                throw std::invalid_argument{"invalid " + std::string{name}};
            }

            return parsed;
        }

        constexpr sink_kind parse_sink(std::string_view value) {
            if (value == "file") {
                return sink_kind::file;
            }

            if (value == "stdout") {
                return sink_kind::stdout;
            }

            if (value == "fd") {
                return sink_kind::fd;
            }

            throw std::invalid_argument{"invalid sink kind"};
        }

        std::vector<log_message> make_dataset(std::size_t count) {
            static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";

            std::vector<log_message> messages;
            messages.reserve(count);

            std::uint64_t state = 0x123456789abcdef0ULL;
            for (std::size_t index = 0; index < count; ++index) {
                log_message message{};
                message.u64 = next_random(state);
                message.u32 = static_cast<std::uint32_t>(next_random(state));
                message.i64 = static_cast<std::int64_t>(next_random(state));
                message.i32 = static_cast<std::int32_t>(next_random(state));

                for (std::size_t i = 0; i + 5 < message.text.size(); ++i) {
                    const auto random = next_random(state);
                    message.text[i] = alphabet[random % alphabet.size()];
                }

                message.text[message.text.size() - 5] = '_';
                message.text[message.text.size() - 4] = 'e';
                message.text[message.text.size() - 3] = 'n';
                message.text[message.text.size() - 2] = 'd';
                message.text.back() = '\0';
                messages.push_back(message);
            }

            return messages;
        }

    }  // namespace

    const benchmark_options& options() noexcept {
        return g_options;
    }

    void parse_options(int* argc, char** argv) {
        int write_index = 1;

        for (int read_index = 1; read_index < *argc; ++read_index) {
            const std::string_view arg{argv[read_index]};

            if (starts_with(arg, "--bench_sink=")) {
                g_options.sink = parse_sink(arg.substr(sizeof("--bench_sink=") - 1));
                continue;
            }

            if (starts_with(arg, "--bench_output_dir=")) {
                g_options.output_dir = arg.substr(sizeof("--bench_output_dir=") - 1);
                continue;
            }

            if (starts_with(arg, "--bench_dataset_size=")) {
                g_options.dataset_size = parse_size(arg.substr(sizeof("--bench_dataset_size=") - 1), "dataset size");
                continue;
            }

            if (starts_with(arg, "--bench_thread_bufsize=")) {
                g_options.thread_bufsize =
                        parse_size(arg.substr(sizeof("--bench_thread_bufsize=") - 1), "thread buffer size");
                continue;
            }

            argv[write_index] = argv[read_index];
            ++write_index;
        }

        *argc = write_index;
    }

    std::string benchmark_name(std::string_view provider_name, std::string_view phase) {
        return fmt::format("{}_{}/{}", provider_name, sink_name(options().sink), phase);
    }

    std::vector<log_message>& dataset() {
        static std::vector<log_message> messages = make_dataset(options().dataset_size);
        return messages;
    }

    std::filesystem::path ensure_output_dir() {
        std::filesystem::create_directories(options().output_dir);
        return options().output_dir;
    }

    std::filesystem::path file_sink_path(std::string_view provider_name) {
        auto output_dir = ensure_output_dir();
        return output_dir / (std::string{provider_name} + ".log");
    }

    std::filesystem::path null_sink_path() {
        return "/dev/null";
    }

}  // namespace unlog_bench
