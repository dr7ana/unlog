#pragma once

#include "unlog/backend/backend.hpp"
#include "unlog/format.hpp"

#include <charconv>
#include <span>

namespace un::log::backend {
    //

    inline constexpr std::string_view ansi_reset{"\x1b[0m"};

    inline constexpr std::string_view color_for_level(log_level level) noexcept {
        switch (level) {
            case log_level::trace:
                return "\x1b[90m";
            case log_level::debug:
                return "\x1b[36m";
            case log_level::info:
                return "\x1b[32m";
            case log_level::warn:
                return "\x1b[33m";
            case log_level::err:
                return "\x1b[31m";
            case log_level::critical:
                return "\x1b[1;31m";
            case log_level::off:
                return "\x1b[2m";
            default:
                [[unlikely]] return std::string_view{};
        }
    }

    constexpr std::chrono::nanoseconds ticks_to_ns(uint64_t ticks) noexcept {
        auto max_ticks = static_cast<uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());
        auto clamped = ticks > max_ticks ? max_ticks : ticks;
        return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(clamped)};
    }

    template <typename DurationT>
    constexpr std::tm local_time(const std::chrono::time_point<std::chrono::system_clock, DurationT>& now) {
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&tt, &tm);
        return tm;
    }

    template <typename DurationT>
    constexpr size_t millis_part(std::chrono::time_point<std::chrono::system_clock, DurationT> timestamp) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count();
        auto normalized = ((ms % 1000) + 1000) % 1000;
        return static_cast<size_t>(normalized);
    }

    struct time_context {
        std::tm tm{};
        size_t millis{0};
        std::array<char, 32> elapsed_storage{};
        size_t elapsed_size{0};

        [[nodiscard]] std::string_view elapsed() const noexcept { return {elapsed_storage.data(), elapsed_size}; }
    };

    inline size_t format_elapsed(std::chrono::nanoseconds elapsed, std::span<char> output) {
        if (elapsed.count() < 0)
            elapsed = std::chrono::nanoseconds{0};

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        auto hours = ms / (60 * 60 * 1000);
        auto minutes = (ms / (60 * 1000)) % 60;
        auto seconds = (ms / 1000) % 60;
        auto millis = ms % 1000;

        auto result = [&] {
            if (hours > 0) {
                return fmt::format_to_n(
                        output.data(), output.size(), "+{}h{:02d}m{:02d}.{:03d}s", hours, minutes, seconds, millis);
            }
            if (minutes > 0) {
                return fmt::format_to_n(output.data(), output.size(), "+{}m{:02d}.{:03d}s", minutes, seconds, millis);
            }
            return fmt::format_to_n(output.data(), output.size(), "+{}.{:03d}s", ms / 1000, millis);
        }();
        return std::min(static_cast<size_t>(result.size), output.size());
    }

    using source_basename_resolver = std::string_view (*)(void*, const char*);

    inline void append_decimal(std::string& out, int64_t value) {
        auto buffer = std::array<char, 24>{};
        auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        out.append(buffer.data(), result.ptr);
    }

    inline void append_two_digits(std::string& out, unsigned value) {
        out.push_back(static_cast<char>('0' + (value / 10u) % 10u));
        out.push_back(static_cast<char>('0' + value % 10u));
    }

    inline void append_three_digits(std::string& out, unsigned value) {
        out.push_back(static_cast<char>('0' + (value / 100u) % 10u));
        out.push_back(static_cast<char>('0' + (value / 10u) % 10u));
        out.push_back(static_cast<char>('0' + value % 10u));
    }

    inline std::string_view render_pattern(
            const sink_entry& sink,
            const log_entry& rec,
            const std::tm& tm,
            size_t millis,
            std::string_view elapsed,
            void* basename_context,
            source_basename_resolver resolve_basename) {
        auto& out = sink.render_buffer;
        out.clear();
        auto required_capacity = sink.pattern.text.size() + rec.message.size() + 64u;
        if (out.capacity() < required_capacity) {
            out.reserve(required_capacity);
        }

        for (auto piece : sink.pattern.pieces) {
            switch (piece.token) {
                case pattern_token::literal:
                    out.append(sink.pattern.text.data() + piece.offset, piece.size);
                    break;
                case pattern_token::percent:
                    out.push_back('%');
                    break;
                case pattern_token::hour:
                    append_two_digits(out, static_cast<unsigned>(tm.tm_hour));
                    break;
                case pattern_token::minute:
                    append_two_digits(out, static_cast<unsigned>(tm.tm_min));
                    break;
                case pattern_token::second:
                    append_two_digits(out, static_cast<unsigned>(tm.tm_sec));
                    break;
                case pattern_token::millis:
                    append_three_digits(out, static_cast<unsigned>(millis));
                    break;
                case pattern_token::elapsed:
                    out.append(elapsed);
                    break;
                case pattern_token::logger_name:
                    out.append(rec.logger_name);
                    break;
                case pattern_token::level:
                    out.append(log_level_string(rec.level));
                    break;
                case pattern_token::source_file:
                    if (rec.source_location.filename) {
                        out.append(
                                resolve_basename ? resolve_basename(basename_context, rec.source_location.filename)
                                                 : std::string_view{rec.source_location.filename});
                    }
                    break;
                case pattern_token::source_line:
                    append_decimal(out, rec.source_location.line);
                    break;
                case pattern_token::message:
                    out.append(rec.message);
                    break;
                case pattern_token::color_start:
                    if (sink.color) {
                        out.append(color_for_level(rec.level));
                    }
                    break;
                case pattern_token::color_end:
                    if (sink.color) {
                        out.append(ansi_reset);
                    }
                    break;
            }
        }
        out.push_back('\n');
        return out;
    }

}  // namespace un::log::backend
