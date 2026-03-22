#pragma once

#include "format.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef UNLOG_DIAGNOSTIC
#define UNLOG_DIAGNOSTIC 0
#endif

namespace un::log {

    using namespace literals;
    using namespace std::literals;

    namespace fs = std::filesystem;

    inline namespace options {
        struct opt {};

        struct clock : public virtual opt {};
        struct steady final : public virtual clock {};
        struct system final : public virtual clock {};

        struct overflow : public virtual opt {};
        struct drop final : public virtual overflow {};
        struct truncate final : public virtual overflow {};

        struct pattern_opt : public virtual opt {};
        template <detail::string_literal Pattern>
        struct pattern : public virtual pattern_opt {
            using type = format_pattern<Pattern>;
        };

        struct max_record_opt : public virtual opt {};
        template <size_t N>
        struct max_record_size final : public virtual max_record_opt {
            static constexpr auto value{N};
        };

        inline constexpr size_t default_max_record_size{4096};
        inline constexpr size_t default_thread_bufsize{1 << 20};
    }  // namespace options

    enum class log_level : uint8_t {
        trace = 0,
        debug = 1,
        info = 2,
        warn = 3,
        err = 4,
        critical = 5,
        off = 6,
    };

    inline constexpr auto log_level_string(log_level level) {
        switch (level) {
            case log_level::trace:
                return "trace"sv;
            case log_level::debug:
                return "debug"sv;
            case log_level::info:
                return "info"sv;
            case log_level::warn:
                return "warn"sv;
            case log_level::err:
                return "error"sv;
            case log_level::critical:
                return "critical"sv;
            case log_level::off:
                return "off"sv;
            default:
                [[unlikely]] return "ERR"sv;
        }
    }

    enum class SinkType : uint8_t { cout, cerr, fd, file, unix_dgram };

    enum Flags : uint8_t { color = 1 << 2 };

    enum class OverflowPolicy : uint8_t { drop, truncate };
    enum class ClockType : uint8_t { steady, system };
    enum class RuntimeMode : uint8_t { single_threaded, threadsafe };
    using channel_id = uint32_t;
    inline constexpr channel_id invalid_channel_id = std::numeric_limits<channel_id>::max();

    struct global_config {
        RuntimeMode mode{RuntimeMode::single_threaded};
        size_t thread_bufsize{options::default_thread_bufsize};
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

    inline constexpr uint8_t default_flags_for_sink(SinkType sink_type) noexcept {
        return (sink_type == SinkType::cout || sink_type == SinkType::cerr) ? Flags::color : 0;
    }

    inline constexpr std::string_view default_pattern_for(uint8_t flags) noexcept {
        return (flags & Flags::color) ? DEFAULT_PATTERN_COLOR : DEFAULT_PATTERN;
    }

    namespace detail {
        template <typename T, typename U = std::remove_cvref_t<T>>
        concept base_opt_type = std::same_as<U, options::clock> || std::same_as<U, options::overflow>;

        template <typename T, typename U = std::remove_cvref_t<T>>
        concept base_val_opt_type = std::same_as<U, options::max_record_opt>;

        template <typename Pattern>
        concept format_pattern_type = requires {
            { Pattern::uses_default_pattern } -> std::convertible_to<bool>;
            { Pattern::text } -> std::convertible_to<std::string_view>;
            { Pattern::requirements } -> std::convertible_to<backend::time_requirements>;
        };

        template <typename T, typename U = std::remove_cvref_t<T>>
        concept channel_opt_type =
                std::derived_from<U, options::clock> || std::derived_from<U, options::overflow> ||
                std::derived_from<U, options::max_record_opt> || std::derived_from<U, options::pattern_opt>;

        template <typename... Arg>
        concept require_channel_opts = (channel_opt_type<Arg> && ...);

        template <typename T, typename... Arg>
        concept at_most_one_is_derived_from = (0 + ... + std::derived_from<std::remove_cvref_t<Arg>, T>) <= 1;

        template <typename... Arg>
        concept valid_channel_opt_pack =
                require_channel_opts<Arg...> && at_most_one_is_derived_from<options::clock, Arg...> &&
                at_most_one_is_derived_from<options::overflow, Arg...> &&
                at_most_one_is_derived_from<options::max_record_opt, Arg...> &&
                at_most_one_is_derived_from<options::pattern_opt, Arg...>;

        template <base_opt_type Base, channel_opt_type Default, typename... Arg>
        struct resolve_opt;

        template <base_opt_type Base, channel_opt_type Default>
        struct resolve_opt<Base, Default> {
            using type = Default;
        };

        template <base_opt_type Base, channel_opt_type Default, typename Head, typename... Tail>
        struct resolve_opt<Base, Default, Head, Tail...> {
            using type = std::conditional_t<
                    std::derived_from<Head, Base>,
                    Head,
                    typename resolve_opt<Base, Default, Tail...>::type>;
        };

        template <base_opt_type Base, channel_opt_type Default, typename... Arg>
            requires valid_channel_opt_pack<Arg...>
        using resolve_opt_t = typename resolve_opt<Base, Default, Arg...>::type;

        template <base_val_opt_type Base, size_t Default, typename... Arg>
        struct resolve_val_opt;

        template <base_val_opt_type Base, size_t Default>
        struct resolve_val_opt<Base, Default> {
            static constexpr auto value = Default;
        };

        template <base_val_opt_type Base, size_t Default, typename Head, typename... Tail>
        struct resolve_val_opt<Base, Default, Head, Tail...> {
            static constexpr auto value{[]() constexpr {
                if constexpr (std::derived_from<Head, Base>)
                    return Head::value;
                return resolve_val_opt<Base, Default, Tail...>::value;
            }()};
        };

        template <base_val_opt_type Base, size_t Default, typename... Arg>
            requires valid_channel_opt_pack<Arg...>
        inline constexpr auto resolve_val_opt_v = resolve_val_opt<Base, Default, Arg...>::value;

        template <typename Default, typename... Arg>
        struct resolve_pattern_opt;

        template <typename Default>
            requires format_pattern_type<Default>
        struct resolve_pattern_opt<Default> {
            using type = Default;
        };

        template <typename Default, typename Head, typename... Tail>
            requires format_pattern_type<Default> && std::derived_from<std::remove_cvref_t<Head>, options::pattern_opt>
        struct resolve_pattern_opt<Default, Head, Tail...> {
            using type = typename std::remove_cvref_t<Head>::type;
        };

        template <typename Default, typename Head, typename... Tail>
            requires format_pattern_type<Default> &&
                     (!std::derived_from<std::remove_cvref_t<Head>, options::pattern_opt>)
        struct resolve_pattern_opt<Default, Head, Tail...> {
            using type = typename resolve_pattern_opt<Default, Tail...>::type;
        };

        template <typename Default, typename... Arg>
            requires valid_channel_opt_pack<Arg...> && format_pattern_type<Default>
        using resolve_pattern_opt_t = typename resolve_pattern_opt<Default, Arg...>::type;

        template <typename Conf>
        concept basic_config_type = requires(const Conf& conf) {
            typename Conf::clock_type;
            typename Conf::overflow_type;
            typename Conf::pattern_type;
            { Conf::max_record_size } -> std::convertible_to<size_t>;
            { Conf::format_requirements } -> std::convertible_to<backend::time_requirements>;
            { conf.name } -> std::convertible_to<std::string_view>;
            { conf.flags } -> std::convertible_to<uint8_t>;
            { conf.format } -> std::convertible_to<std::string_view>;
            { conf.sink_type } -> std::convertible_to<SinkType>;
            { conf.filename } -> std::same_as<const std::optional<fs::path>&>;
            { conf.output_fd } -> std::same_as<const std::optional<int>&>;
            { conf.unix_dgram_path } -> std::same_as<const std::optional<fs::path>&>;
        };

        template <basic_config_type Conf>
        inline constexpr auto config_clock_type_v =
                std::derived_from<typename Conf::clock_type, options::system> ? ClockType::system : ClockType::steady;

        template <basic_config_type Conf>
        inline constexpr auto config_overflow_policy_v =
                std::derived_from<typename Conf::overflow_type, options::truncate> ? OverflowPolicy::truncate
                                                                                   : OverflowPolicy::drop;

        template <basic_config_type Conf>
        inline constexpr SinkType config_sink_type(const Conf& conf) {
            return conf.sink_type;
        }

        template <basic_config_type Conf>
        inline constexpr std::optional<fs::path> config_filename(const Conf& conf) {
            return conf.filename;
        }

        template <basic_config_type Conf>
        inline constexpr std::optional<int> config_output_fd(const Conf& conf) {
            return conf.output_fd;
        }

        template <basic_config_type Conf>
        inline constexpr std::optional<fs::path> config_unix_dgram_path(const Conf& conf) {
            return conf.unix_dgram_path;
        }

        template <basic_config_type Conf>
        inline constexpr backend::time_requirements config_format_requirements(const Conf&) noexcept {
            return Conf::format_requirements;
        }
    }  // namespace detail

    template <typename... Arg>
        requires detail::valid_channel_opt_pack<Arg...>
    struct config {
        using clock_type = detail::resolve_opt_t<options::clock, options::steady, Arg...>;
        using overflow_type = detail::resolve_opt_t<options::overflow, options::drop, Arg...>;
        using pattern_type = detail::resolve_pattern_opt_t<default_pattern_type, Arg...>;

        static constexpr auto max_record_size =
                detail::resolve_val_opt_v<options::max_record_opt, default_max_record_size, Arg...>;
        static constexpr auto format_requirements = pattern_type::requirements;

        std::string_view name{};
        uint8_t flags{};
        std::string_view format{};
        SinkType sink_type{SinkType::cout};
        std::optional<fs::path> filename{};
        std::optional<int> output_fd{};
        std::optional<fs::path> unix_dgram_path{};

        config() = delete;

        explicit constexpr config(
                std::string_view _name,
                SinkType _sink_type,
                uint8_t _flags,
                std::optional<fs::path> _filename = std::nullopt,
                std::optional<int> _output_fd = std::nullopt,
                std::optional<fs::path> _unix_dgram_path = std::nullopt) :
                name{_name},
                flags{_flags},
                format{[&]() constexpr {
                    if constexpr (pattern_type::uses_default_pattern) {
                        return default_pattern_for(_flags);
                    }

                    return pattern_type::text;
                }()},
                sink_type{_sink_type},
                filename{std::move(_filename)},
                output_fd{std::move(_output_fd)},
                unix_dgram_path{std::move(_unix_dgram_path)} {
            switch (sink_type) {
                case SinkType::cout:
                case SinkType::cerr:
                    break;
                case SinkType::file:
                    if (!filename.has_value() || filename->empty())
                        throw std::invalid_argument{"file sink requires filename"};
                    break;
                case SinkType::fd:
                    if (!output_fd.has_value())
                        throw std::invalid_argument{"fd sink requires output_fd"};
                    break;
                case SinkType::unix_dgram:
                    if (!unix_dgram_path.has_value())
                        throw std::invalid_argument{"unix_dgram sink requires unix_dgram_path"};
                    break;
            }
        }

        static constexpr auto make(std::string_view n = "unlog"sv) {
            return config{n, SinkType::cout, default_flags_for_sink(SinkType::cout)};
        }

        static constexpr auto make_stderr(std::string_view n = "unlog"sv) {
            return config{n, SinkType::cerr, default_flags_for_sink(SinkType::cerr)};
        }

        static constexpr auto make_file(fs::path file, std::string_view n = "unlog"sv) {
            return config{n, SinkType::file, 0, std::move(file)};
        }

        static constexpr auto make_fd(int fd, std::string_view n = "unlog"sv) {
            return config{n, SinkType::fd, 0, std::nullopt, fd};
        }

        static constexpr auto make_unix_dgram(fs::path path, std::string_view n = "unlog"sv) {
            return config{n, SinkType::unix_dgram, 0, std::nullopt, std::nullopt, std::move(path)};
        }

        constexpr bool color() const noexcept { return flags & Flags::color; }
    };

}  // namespace un::log
