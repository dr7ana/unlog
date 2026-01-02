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

    // stdout, stderr, file, async (threadpool)
    enum class Type : uint8_t { cout, cerr, File };

    enum Flags : uint8_t { threadsafe = 1 << 1, color = 1 << 2, async = 1 << 3 };

    inline constexpr auto type_string(Type t) {
        switch (t) {
            case Type::cout:
                return "cout"sv;
            case Type::cerr:
                return "cerr"sv;
            case Type::File:
                return "file"sv;
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
        Type type;
        uint8_t flags;
        uint8_t threads;
        uint32_t pool_threads;
        std::optional<std::string> format{std::nullopt};
        std::optional<fs::path> filename{std::nullopt};

        Config() = delete;

        Config(std::string_view _name,
               Type _type,
               uint8_t _flags,
               uint8_t _threads,
               uint32_t _pool_size,
               std::optional<std::string> _format = std::nullopt) :
                name{_name.data(), _name.size()},
                type{_type},
                flags{_flags},
                threads{_threads},
                pool_threads{_pool_size},
                format{std::move(_format)} {
            if (type == Type::File)
                throw std::invalid_argument{"File logger must have filename"};
        }

        Config(std::string_view _name,
               fs::path _filename,
               Type _type,
               uint8_t _flags,
               uint8_t _threads,
               uint32_t _pool_size,
               std::optional<std::string> _format = std::nullopt) :
                name{_name.data(), _name.size()},
                type{_type},
                flags{_flags},
                threads{_threads},
                pool_threads{_pool_size},
                format{std::move(_format)},
                filename{std::move(_filename)} {
            if (type != Type::File)
                throw std::invalid_argument{"File logger must use file type"};
            if (filename->empty())
                throw std::invalid_argument{"File logger must have a non-empty filename"};
        }

        static Config make_default(std::string_view n = "unlog"sv) { return Config{n, Type::cout, Flags::color, 0, 0}; }

        static Config make_async(std::string_view n = "unlog"sv, uint8_t thread_count = 1, uint32_t pool_size = 8192) {
            return Config{n, Type::cout, Flags::color | Flags::threadsafe | Flags::async, thread_count, pool_size};
        }

        static Config make_file(const fs::path& file, std::string_view n = "unlog"sv) {
            return Config{n, file, Type::File, Flags::threadsafe, 0, 0};
        }

        constexpr bool threadsafe() const { return flags & Flags::threadsafe; }
        constexpr bool color() const { return flags & Flags::color; }
        constexpr bool async() const { return flags & Flags::async; }
        constexpr bool cout_log() const { return type == Type::cout; }
        constexpr bool cerr_log() const { return type == Type::cerr; }
        constexpr bool file_log() const { return type == Type::File && filename.has_value(); }

        fs::path file() const { return filename.value_or(fs::path{"INVALID"}); }

        std::string to_string() const { return "Config[ name={} | type={} ]"_format(name, type_string(type)); }

        static constexpr auto to_string_formattable = true;
    };

}  // namespace un::log
