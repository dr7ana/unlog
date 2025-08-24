#pragma once

#include "format.hpp"

#include <bitset>

namespace un::log {

    using namespace literals;
    using namespace std::literals;

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
        }
    }

    /*  Config Fields:
        - Bitfield flags (0 vs 1)
            - threadsafe (mt vs st)
            - color (yes vs no)
            - async (no vs yes)
    */
    struct Config {
        cspan name;
        Type type;
        uint8_t flags;
        uint8_t threads;
        uint32_t pool_threads;
        std::optional<std::string> format{std::nullopt};
        std::optional<std::string> filename{std::nullopt};

        Config() = delete;

        constexpr Config(cspan _name, Type _type, uint8_t _flags, uint8_t _threads, uint8_t _pool_size) :
                name{_name}, type{_type}, flags{_flags}, threads{_threads}, pool_threads{_pool_size} {
            // File type must have filename, vice versa
            if ((type == Type::File) != filename.has_value())
                throw std::invalid_argument{"File logger must have filename, and vice versa"};
        }

        template <char_view_type T>
        static constexpr Config make_default(T n = "unlog"_sp) {
            return Config{std::move(n), Type::cout, Flags::color, 0, 0};
        }

        template <char_view_type T>
        static constexpr Config make_async(T n = "unlog"_sp, uint8_t thread_count = 1, uint32_t pool_size = 8192) {
            return Config{n, Type::cout, Flags::color | Flags::threadsafe | Flags::async, thread_count, pool_size};
        }

        template <char_view_type T>
        static constexpr Config make_file(cspan file, T n = "unlog"_sp) {
            return Config{n, Type::cout, Flags::color | Flags::threadsafe | Flags::async, 0, 0, file};
        }

        constexpr bool threadsafe() const { return flags & Flags::threadsafe; }
        constexpr bool color() const { return flags & Flags::color; }
        constexpr bool async() const { return flags & Flags::async; }
        constexpr bool cout_log() const { return type == Type::cout; }
        constexpr bool cerr_log() const { return type == Type::cerr; }
        constexpr bool file_log() const { return type == Type::File && filename.has_value(); }

        std::string file() const { return filename.value_or("INVALID"s); }

        std::string to_string() const { return "Config[ name={} | type={} ]"_format(name, type_string(type)); }

        static constexpr auto to_string_formattable = true;
    };

}  // namespace un::log
