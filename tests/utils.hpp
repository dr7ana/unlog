#pragma once

#include "unlog.hpp"

#include "unlog/backend/sinks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <mutex>
#include <ostream>
#include <utility>
#include <vector>

namespace un::log::test {

    using namespace un::log::literals;

    void get_runtime_backend(const std::function<void()>& fn);
    void reset_runtime_for_test();

    struct test_helper {
        template <typename... Opt>
            requires detail::valid_channel_opt_pack<Opt...>
        static auto make_config(
                std::string_view name = "unlog"sv,
                SinkType sink_type = SinkType::cout,
                uint8_t flags = Flags::color,
                std::optional<std::string> format = std::nullopt,
                std::optional<fs::path> filename = std::nullopt,
                std::optional<int> output_fd = std::nullopt,
                std::optional<fs::path> unix_dgram_path = std::nullopt) {
            return config<Opt...>{
                    name,
                    sink_type,
                    flags,
                    std::move(format),
                    std::move(filename),
                    std::move(output_fd),
                    std::move(unix_dgram_path),
            };
        }

        static void reset_runtime_state() { reset_runtime_for_test(); }
    };

    struct runtime_state_guard {
        runtime_state_guard() { test_helper::reset_runtime_state(); }

        ~runtime_state_guard() { test_helper::reset_runtime_state(); }
    };

    struct util {
        static std::stringstream stream;
        static std::mutex stream_mutex;

        struct capture_sink final : backend::sink {
            void write(std::string_view line) override {
                std::lock_guard lock{util::stream_mutex};
                util::stream << line;
            }

            void flush() override {
                std::lock_guard lock{util::stream_mutex};
                util::stream.flush();
            }
        };

        static auto reset() {
            unlog::flush();
            std::lock_guard lock{stream_mutex};
            stream = {};
            stream.clear();
        }

        static auto capture_test_logs(log_level level = get_default_level()) {
            reset();
            set_default_level(level);
            auto conf = config<>::make("capture");
            detail::add_sink(conf, std::make_shared<capture_sink>());
        }

        template <detail::basic_config_type Conf>
        static auto capture_test_logs(const Conf& conf, log_level level = get_default_level()) {
            reset();
            set_default_level(level);
            detail::add_sink(conf, std::make_shared<capture_sink>());
        }

        static std::string captured_output() {
            unlog::flush();
            std::lock_guard lock{stream_mutex};
            return stream.str();
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_CONTAINS {
        REQUIRE_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_CONTAINS {
        CHECK_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_NOT_CONTAINS {
        REQUIRE_NOT_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.contains(msg) == false);
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_NOT_CONTAINS {
        CHECK_NOT_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.contains(msg) == false);
        }
    };

    struct REQUIRE_EMPTY {
        REQUIRE_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.empty());
        }
    };

    struct CHECK_EMPTY {
        CHECK_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.empty());
        }
    };

    // deduction guides
    template <typename T>
    REQUIRE_CONTAINS(T msg) -> REQUIRE_CONTAINS<T>;
    template <typename T>
    CHECK_CONTAINS(T msg) -> CHECK_CONTAINS<T>;
    template <typename T>
    REQUIRE_NOT_CONTAINS(T msg) -> REQUIRE_NOT_CONTAINS<T>;
    template <typename T>
    CHECK_NOT_CONTAINS(T msg) -> CHECK_NOT_CONTAINS<T>;

}  // namespace un::log::test
