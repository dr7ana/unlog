#pragma once

#include "unlog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <ostream>

namespace un::log::test {

    using namespace un::log::literals;

    struct util {
        static std::stringstream stream;

        static auto reset() {
            stream = {};
            stream.clear();
        }

        static auto capture_test_logs(LogLevel level = get_default_level()) {
            unlog::flush();  // clear any previous test case logs in buffer
            reset();
            set_default_level(level);
            detail::add_sink(std::make_shared<spdlog::sinks::ostream_sink_mt>(stream));
        }

        static auto capture_test_logs(const Config& conf, LogLevel level = get_default_level()) {
            unlog::flush();  // clear any previous test case logs in buffer
            reset();
            set_default_level(level);
            detail::add_sink(conf, std::make_shared<spdlog::sinks::ostream_sink_mt>(stream));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_CONTAINS {
        REQUIRE_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            REQUIRE(util::stream.str().contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_CONTAINS {
        CHECK_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            CHECK(util::stream.str().contains(msg));
        }
    };

    struct REQUIRE_EMPTY {
        REQUIRE_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            REQUIRE(util::stream.str().empty());
        }
    };

    struct CHECK_EMPTY {
        CHECK_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            CHECK(util::stream.str().empty());
        }
    };

    // deduction guides
    template <typename T>
    REQUIRE_CONTAINS(T msg) -> REQUIRE_CONTAINS<T>;
    template <typename T>
    CHECK_CONTAINS(T msg) -> CHECK_CONTAINS<T>;

}  // namespace un::log::test
