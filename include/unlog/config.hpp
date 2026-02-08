#pragma once

#include "format.hpp"

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace un::log {

    using namespace literals;
    using namespace std::literals;

    namespace fs = std::filesystem;

    /*
     * Config type layout.
     *
     * Primary config types:
     * - `config<options::sqpoll_live, ...>` (sqpoll config)
     * - `config<options::file, ...>` (file config)
     *
     * Common fields across both:
     * - `name`
     * - `flags`
     * - `format`
     * - compile-time resolved options:
     *   - `clock_type`
     *   - `overflow_type`
     *   - `max_record_size`
     *   - `thread_bufsize`
     *
     * Fields exclusive to sqpoll config:
     * - `sqpoll_queue_depth`
     * - `strict_nonblocking`
     * - endpoint fields for fd/dgram path:
     *   - `sink_type`
     *   - `output_fd`
     *   - `unix_dgram_path`
     *
     * Fields exclusive to file config:
     * - `filename`
     *
     * Invariants this layout enforces:
     * - file config does not carry fd/dgram endpoint fields
     * - sqpoll config does not carry `filename`
     */

    inline namespace options {
        // EBO base-class tags for compile-time options
        struct opt {};

        // memory policy: first-class configurable option
        struct memory : opt {};
        struct sqpoll_live final : public virtual memory {};
        struct file final : public virtual memory {};

        // clock type
        struct clock : public virtual opt {};
        struct steady final : public virtual clock {};
        struct system final : public virtual clock {};

        // overflow policy
        struct overflow : public virtual opt {};
        struct drop final : public virtual overflow {};
        struct truncate final : public virtual overflow {};

        // max log record size
        struct max_record_opt : public virtual opt {};
        template <size_t N>
        struct max_record_size final : public virtual max_record_opt {
            static constexpr auto value{N};
        };

        // thread-local ring buffer size
        struct thread_bufsize_opt : public virtual opt {};
        template <size_t N>
        struct thread_bufsize final : public virtual thread_bufsize_opt {
            static constexpr auto value{N};
        };

        inline constexpr size_t default_sqpoll_queue_depth{4096};
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

    namespace detail {
        template <typename T, typename U = std::remove_cvref_t<T>>
        concept base_opt_type = std::same_as<U, options::memory> || std::same_as<U, options::clock> ||
                                std::same_as<U, options::overflow>;

        template <typename T, typename U = std::remove_cvref_t<T>>
        concept base_val_opt_type =
                std::same_as<U, options::max_record_opt> || std::same_as<U, options::thread_bufsize_opt>;

        template <typename T, typename U = std::remove_cvref_t<T>>
        concept val_opt_arg_type =
                (std::derived_from<U, options::max_record_opt> || std::derived_from<U, options::thread_bufsize_opt>) &&
                requires { U::value; };

        template <typename T, typename U = std::remove_cvref_t<T>>
        concept tagged_opt_type = std::derived_from<U, options::opt>;

        template <typename... Arg>
        concept require_tagged_opts = (tagged_opt_type<Arg> && ...);

        template <typename T, typename... Arg>
        concept at_most_one_is_derived_from = (0 + ... + std::derived_from<std::remove_cvref_t<Arg>, T>) <= 1;

        template <typename... Arg>
        concept valid_opt_pack = require_tagged_opts<Arg...> && at_most_one_is_derived_from<options::memory, Arg...> &&
                                 at_most_one_is_derived_from<options::clock, Arg...> &&
                                 at_most_one_is_derived_from<options::overflow, Arg...> &&
                                 at_most_one_is_derived_from<options::max_record_opt, Arg...> &&
                                 at_most_one_is_derived_from<options::thread_bufsize_opt, Arg...>;

        /* Resolver templates for typed options */
        template <base_opt_type Base, tagged_opt_type Default, typename... Arg>
        struct resolve_opt;

        template <base_opt_type Base, tagged_opt_type Default>
        struct resolve_opt<Base, Default> {
            using type = Default;
        };

        template <base_opt_type Base, tagged_opt_type Default, typename Head, typename... Tail>
        struct resolve_opt<Base, Default, Head, Tail...> {
            //
            using type = std::conditional_t<
                    std::derived_from<Head, Base>,
                    Head,
                    typename resolve_opt<Base, Default, Tail...>::type>;
        };

        template <base_opt_type Base, tagged_opt_type Default, typename... Arg>
            requires valid_opt_pack<Arg...>
        using resolve_opt_t = typename resolve_opt<Base, Default, Arg...>::type;

        /* Resolver templates for value options */
        template <base_val_opt_type Base, size_t Default, typename... Arg>
        struct resolve_val_opt;

        template <base_val_opt_type Base, size_t Default>
        struct resolve_val_opt<Base, Default> {
            static constexpr auto value = Default;
        };

        template <base_val_opt_type Base, size_t Default, typename Head, typename... Tail>
        struct resolve_val_opt<Base, Default, Head, Tail...> {
            static constexpr auto value{[]() constexpr {
                if constexpr (std::derived_from<Head, Base>) {
                    return Head::value;
                }
                return resolve_val_opt<Base, Default, Tail...>::value;
            }()};
        };

        template <base_val_opt_type Base, size_t Default, typename... Arg>
            requires valid_opt_pack<Arg...>
        inline constexpr auto resolve_val_opt_v = resolve_val_opt<Base, Default, Arg...>::value;

        template <typename Conf>
        concept basic_config_type = requires(const Conf& conf) {
            typename Conf::memory_type;
            typename Conf::clock_type;
            typename Conf::overflow_type;
            { Conf::max_record_size } -> std::convertible_to<size_t>;
            { Conf::thread_bufsize } -> std::convertible_to<size_t>;
            { conf.name } -> std::convertible_to<std::string_view>;
            { conf.flags } -> std::convertible_to<uint8_t>;
            { conf.format } -> std::convertible_to<std::string_view>;
        };

        template <typename Conf>
        concept sqpoll_config_type =
                basic_config_type<Conf> && std::derived_from<typename Conf::memory_type, options::sqpoll_live> &&
                requires(const Conf& conf) {
                    { conf.sink_type } -> std::convertible_to<SinkType>;
                    { conf.strict_nonblocking } -> std::convertible_to<bool>;
                    { conf.sqpoll_queue_depth } -> std::convertible_to<size_t>;
                    { conf.output_fd } -> std::same_as<const std::optional<int>&>;
                    { conf.unix_dgram_path } -> std::same_as<const std::optional<fs::path>&>;
                };

        template <typename Conf>
        concept file_config_type =
                basic_config_type<Conf> && std::derived_from<typename Conf::memory_type, options::file> &&
                requires(const Conf& conf) {
                    { conf.filename } -> std::same_as<const fs::path&>;
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
            if constexpr (sqpoll_config_type<Conf>)
                return conf.sink_type;
            else if constexpr (file_config_type<Conf>)
                return SinkType::file;
            else
                return SinkType::cout;
        }

        template <file_config_type Conf>
        inline constexpr std::optional<fs::path> config_filename(const Conf& conf) {
            return conf.filename;
        }

        template <basic_config_type Conf>
            requires(!file_config_type<Conf>)
        inline constexpr std::optional<fs::path> config_filename(const Conf&) {
            return std::nullopt;
        }

        template <sqpoll_config_type Conf>
        inline constexpr bool config_strict_nonblocking(const Conf& conf) {
            return conf.strict_nonblocking;
        }

        template <basic_config_type Conf>
            requires(!sqpoll_config_type<Conf>)
        inline constexpr bool config_strict_nonblocking(const Conf&) {
            return true;
        }

        template <sqpoll_config_type Conf>
        inline constexpr size_t config_sqpoll_queue_depth(const Conf& conf) {
            return conf.sqpoll_queue_depth;
        }

        template <basic_config_type Conf>
            requires(!sqpoll_config_type<Conf>)
        inline constexpr size_t config_sqpoll_queue_depth(const Conf&) {
            return options::default_sqpoll_queue_depth;
        }

        template <sqpoll_config_type Conf>
        inline constexpr std::optional<int> config_output_fd(const Conf& conf) {
            return conf.output_fd;
        }

        template <basic_config_type Conf>
            requires(!sqpoll_config_type<Conf>)
        inline constexpr std::optional<int> config_output_fd(const Conf&) {
            return std::nullopt;
        }

        template <sqpoll_config_type Conf>
        inline constexpr std::optional<fs::path> config_unix_dgram_path(const Conf& conf) {
            return conf.unix_dgram_path;
        }

        template <basic_config_type Conf>
            requires(!sqpoll_config_type<Conf>)
        inline constexpr std::optional<fs::path> config_unix_dgram_path(const Conf&) {
            return std::nullopt;
        }

    }  // namespace detail

    inline std::string resolve_format_pattern(uint8_t flags, std::optional<std::string> format_override) {
        if (format_override.has_value())
            return std::move(*format_override);

        return (flags & Flags::color) ? DEFAULT_PATTERN_COLOR : DEFAULT_PATTERN;
    }

    template <typename... Arg>
        requires detail::valid_opt_pack<Arg...>
    struct config {
        using memory_type = detail::resolve_opt_t<options::memory, options::sqpoll_live, Arg...>;
        using clock_type = detail::resolve_opt_t<options::clock, options::steady, Arg...>;
        using overflow_type = detail::resolve_opt_t<options::overflow, options::drop, Arg...>;

        static constexpr auto max_record_size =
                detail::resolve_val_opt_v<options::max_record_opt, default_max_record_size, Arg...>;
        static constexpr auto thread_bufsize =
                detail::resolve_val_opt_v<options::thread_bufsize_opt, default_thread_bufsize, Arg...>;

        std::string name{};
        uint8_t flags{};
        std::string format{};

        config() = delete;

        explicit config(std::string_view _name, uint8_t _flags, std::optional<std::string> _format = std::nullopt) :
                name{_name.data(), _name.size()},
                flags{_flags},
                format{resolve_format_pattern(_flags, std::move(_format))} {}

        static auto make_sqpoll(std::string_view n = "unlog"sv);
        static auto make_file(fs::path file, std::string_view n = "unlog"sv);

        constexpr bool color() const noexcept { return flags & Flags::color; }
    };

    template <typename... Arg>
        requires detail::valid_opt_pack<options::sqpoll_live, Arg...>
    struct basic_sqpoll_config final : public config<options::sqpoll_live, Arg...> {
        using base_type = config<options::sqpoll_live, Arg...>;

        SinkType sink_type{SinkType::cout};
        bool strict_nonblocking{true};
        size_t sqpoll_queue_depth{options::default_sqpoll_queue_depth};
        std::optional<int> output_fd{std::nullopt};             // required for SinkType::fd
        std::optional<fs::path> unix_dgram_path{std::nullopt};  // required for SinkType::unix_dgram

        basic_sqpoll_config() = delete;

        explicit basic_sqpoll_config(
                std::string_view _name,
                SinkType _sink_type,
                uint8_t _flags,
                std::optional<std::string> _format = std::nullopt,
                std::optional<int> _output_fd = std::nullopt,
                std::optional<fs::path> _unix_dgram_path = std::nullopt) :
                base_type{_name, _flags, std::move(_format)},
                sink_type{_sink_type},
                output_fd{std::move(_output_fd)},
                unix_dgram_path{std::move(_unix_dgram_path)} {
            if (sink_type == SinkType::file)
                throw std::invalid_argument{"sqpoll config does not accept file sink"};

            if (sink_type == SinkType::fd && !output_fd.has_value())
                throw std::invalid_argument{"fd sink requires output_fd"};

            if (sink_type == SinkType::unix_dgram && !unix_dgram_path.has_value())
                throw std::invalid_argument{"unix_dgram sink requires unix_dgram_path"};
        }
    };

    template <typename... Arg>
        requires detail::valid_opt_pack<options::file, Arg...>
    struct basic_file_config final : public config<options::file, Arg...> {
        using base_type = config<options::file, Arg...>;

        fs::path filename{};

        basic_file_config() = delete;

        explicit basic_file_config(
                std::string_view _name,
                fs::path _filename,
                uint8_t _flags = 0,
                std::optional<std::string> _format = std::nullopt) :
                base_type{_name, _flags, std::move(_format)}, filename{std::move(_filename)} {
            if (filename.empty())
                throw std::invalid_argument{"file config requires a non-empty filename"};
        }
    };

    template <typename... Arg>
        requires detail::valid_opt_pack<Arg...>
    auto config<Arg...>::make_sqpoll(std::string_view n) {
        return basic_sqpoll_config<Arg...>{n, SinkType::cout, Flags::color};
    }

    template <typename... Arg>
        requires detail::valid_opt_pack<Arg...>
    auto config<Arg...>::make_file(fs::path file, std::string_view n) {
        return basic_file_config<Arg...>{n, std::move(file), 0};
    }

}  // namespace un::log
