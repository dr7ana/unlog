#pragma once

#include "format.hpp"

#include <bitset>
#include <filesystem>
#include <optional>

namespace un::log {

    using namespace literals;
    using namespace std::literals;

    namespace fs = std::filesystem;

    using LogLevel = spdlog::level::level_enum;

    // sink selection
    enum class SinkType : uint8_t { cout, cerr, file, fd, unix_dgram };

    enum Flags : uint8_t { threadsafe = 1 << 1, color = 1 << 2, async = 1 << 3 };

    enum class MemoryPolicy : uint8_t { sqpoll_live, memory_only };
    enum class OverflowPolicy : uint8_t { drop, truncate };
    enum class ClockType : uint8_t { steady, system };

    struct BackendOptions {
        MemoryPolicy memory_policy{MemoryPolicy::sqpoll_live};
        OverflowPolicy overflow_policy{OverflowPolicy::drop};
        ClockType timestamp_mode{ClockType::steady};
        size_t thread_bufsize{1u << 20};  // 1 MiB
        uint32_t max_record_size{4096};
        bool strict_nonblocking{true};
        uint32_t sqpoll_queue_depth{4096};
        std::optional<int> output_fd{std::nullopt};             // required for SinkType::fd
        std::optional<fs::path> unix_dgram_path{std::nullopt};  // required for SinkType::unix_dgram
    };

    inline constexpr auto sink_type_string(SinkType t) {
        switch (t) {
            case SinkType::cout:
                return "cout"sv;
            case SinkType::cerr:
                return "cerr"sv;
            case SinkType::file:
                return "file"sv;
            case SinkType::fd:
                return "fd"sv;
            case SinkType::unix_dgram:
                return "unix_dgram"sv;
            default:
                [[unlikely]] return "ERR"sv;
        }
    }

    /*  Config Fields:
        - Bitfield flags (0 vs 1)
            - threadsafe (mt vs st)
            - color (yes vs no)
            - async (no vs yes)
    */
    struct Config {
        std::string name;
        union {
            SinkType sink_type;
            SinkType type;
        };
        uint8_t flags;
        uint8_t threads;
        uint32_t pool_threads;
        std::optional<std::string> format{std::nullopt};
        std::optional<fs::path> filename{std::nullopt};
        BackendOptions backend{};

        Config() = delete;

        Config(std::string_view _name,
               SinkType _sink_type,
               uint8_t _flags,
               uint8_t _threads,
               uint32_t _pool_size,
               std::optional<std::string> _format = std::nullopt,
               BackendOptions _backend = {}) :
                name{_name.data(), _name.size()},
                sink_type{_sink_type},
                flags{_flags},
                threads{_threads},
                pool_threads{_pool_size},
                format{std::move(_format)},
                backend{std::move(_backend)} {
            if (sink_type == SinkType::file)
                throw std::invalid_argument{"File logger must have filename"};
            validate_sink();
        }

        Config(std::string_view _name,
               fs::path _filename,
               SinkType _sink_type,
               uint8_t _flags,
               uint8_t _threads,
               uint32_t _pool_size,
               std::optional<std::string> _format = std::nullopt,
               BackendOptions _backend = {}) :
                name{_name.data(), _name.size()},
                sink_type{_sink_type},
                flags{_flags},
                threads{_threads},
                pool_threads{_pool_size},
                format{std::move(_format)},
                filename{std::move(_filename)},
                backend{std::move(_backend)} {
            if (sink_type != SinkType::file)
                throw std::invalid_argument{"File logger must use file type"};
            if (filename->empty())
                throw std::invalid_argument{"File logger must have a non-empty filename"};
            validate_sink();
        }

        static Config make_default(std::string_view n = "unlog"sv) {
            return Config{n, SinkType::cout, Flags::color, 0, 0};
        }

        static Config make_async(std::string_view n = "unlog"sv, uint8_t thread_count = 1, uint32_t pool_size = 8192) {
            return Config{n, SinkType::cout, Flags::color | Flags::threadsafe | Flags::async, thread_count, pool_size};
        }

        static Config make_file(const fs::path& file, std::string_view n = "unlog"sv) {
            return Config{n, file, SinkType::file, Flags::threadsafe, 0, 0};
        }

        constexpr bool threadsafe() const { return flags & Flags::threadsafe; }
        constexpr bool color() const { return flags & Flags::color; }
        constexpr bool async() const { return flags & Flags::async; }
        constexpr bool cout_log() const { return sink_type == SinkType::cout; }
        constexpr bool cerr_log() const { return sink_type == SinkType::cerr; }
        constexpr bool file_log() const { return sink_type == SinkType::file && filename.has_value(); }
        constexpr bool fd_log() const { return sink_type == SinkType::fd; }
        constexpr bool unix_dgram_log() const { return sink_type == SinkType::unix_dgram; }

        fs::path file() const { return filename.value_or(fs::path{"INVALID"}); }

        std::string to_string() const {
            return "Config[ name={} | type={} ]"_format(name, sink_type_string(sink_type));
        }

        static constexpr auto to_string_formattable = true;

      private:
        void validate_sink() const {
            if (sink_type == SinkType::fd && !backend.output_fd.has_value())
                throw std::invalid_argument{"fd sink requires backend.output_fd"};

            if (sink_type == SinkType::unix_dgram && !backend.unix_dgram_path.has_value())
                throw std::invalid_argument{"unix_dgram sink requires backend.unix_dgram_path"};
        }
    };

}  // namespace un::log
