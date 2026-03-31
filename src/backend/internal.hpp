#pragma once

#include "unlog/backend/backend.hpp"
#include "unlog/format.hpp"

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
        std::string elapsed;
    };

    inline std::string format_elapsed(std::chrono::nanoseconds elapsed) {
        if (elapsed.count() < 0)
            elapsed = std::chrono::nanoseconds{0};

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        auto hours = ms / (60 * 60 * 1000);
        auto minutes = (ms / (60 * 1000)) % 60;
        auto seconds = (ms / 1000) % 60;
        auto millis = ms % 1000;

        if (hours > 0)
            return "+{}h{:02d}m{:02d}.{:03d}s"_format(hours, minutes, seconds, millis);

        if (minutes > 0)
            return "+{}m{:02d}.{:03d}s"_format(minutes, seconds, millis);

        return "+{}.{:03d}s"_format(ms / 1000, millis);
    }

    inline std::string render_pattern(
            std::string_view pattern,
            bool color,
            const log_entry& rec,
            const std::tm& tm,
            size_t millis,
            std::string_view elapsed) {
        std::string out;
        out.reserve(pattern.size() + rec.message.size() + 64);

        for (size_t i = 0; i < pattern.size(); ++i) {
            auto ch = pattern[i];
            if (ch != '%') {
                out.push_back(ch);
                continue;
            }

            if ((i + 1) >= pattern.size()) {
                out.push_back('%');
                break;
            }

            auto tok = pattern[++i];
            switch (tok) {
                case '%':
                    out.push_back('%');
                    break;
                case 'H':
                    "{:02d}"_format_to(out, tm.tm_hour);
                    break;
                case 'M':
                    "{:02d}"_format_to(out, tm.tm_min);
                    break;
                case 'S':
                    "{:02d}"_format_to(out, tm.tm_sec);
                    break;
                case 'e':
                    "{:03d}"_format_to(out, millis);
                    break;
                case '*':
                    "{}"_format_to(out, elapsed);
                    break;
                case 'n':
                    "{}"_format_to(out, rec.logger_name);
                    break;
                case 'l':
                    "{}"_format_to(out, log_level_string(rec.level));
                    break;
                case 'g':
                    if (rec.source_location.filename)
                        "{}"_format_to(out, rec.source_location.filename);
                    break;
                case '#':
                    "{}"_format_to(out, rec.source_location.line);
                    break;
                case 'v':
                    "{}"_format_to(out, rec.message);
                    break;
                case '^':
                    if (color)
                        "{}"_format_to(out, color_for_level(rec.level));
                    break;
                case '$':
                    if (color)
                        "{}"_format_to(out, ansi_reset);
                    break;
                default:
                    out.push_back('%');
                    out.push_back(tok);
                    break;
            }
        }

        return out;
    }

    struct line_cache_entry {
        std::string_view pattern;
        bool color{false};
        bool with_newline{false};
        std::string line;
    };

    inline constexpr bool same_cache_key(
            const line_cache_entry& entry, std::string_view pattern, bool color, bool with_newline) noexcept {
        return entry.color == color && entry.with_newline == with_newline && entry.pattern == pattern;
    }

    inline constexpr std::string_view format_cache_line(
            std::vector<line_cache_entry>& line_cache,
            std::string_view pattern,
            bool color,
            bool with_newline,
            const log_entry& rec,
            const std::tm& tm,
            size_t millis,
            std::string_view elapsed) {
        for (auto& entry : line_cache) {
            if (same_cache_key(entry, pattern, color, with_newline))
                return entry.line;
        }

        auto line = render_pattern(pattern, color, rec, tm, millis, elapsed);
        if (with_newline)
            line.push_back('\n');

        line_cache.push_back(
                line_cache_entry{
                        .pattern = pattern,
                        .color = color,
                        .with_newline = with_newline,
                        .line = std::move(line),
                });
        return line_cache.back().line;
    }

}  // namespace un::log::backend
