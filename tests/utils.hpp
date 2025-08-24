#pragma once

#include "unlog.hpp"

#include <spdlog/sinks/ostream_sink.h>

#include <catch2/catch_test_macros.hpp>

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
            reset();
            reset_level(level);
            detail::add_sink(std::make_shared<spdlog::sinks::ostream_sink_mt>(stream));
        }

        static auto CHECK_CONTAINS(const std::string& msg) {
            INFO("Contents: " << stream.str());
            CHECK(stream.str().contains(msg));
        }
        static auto REQUIRE_CONTAINS(const std::string& msg) {
            INFO("Contents: " << stream.str());
            REQUIRE(stream.str().contains(msg));
        }

        static auto CHECK_EMPTY() {
            INFO("Contents: " << stream.str());
            CHECK(stream.str().empty());
        }
        static auto REQUIRE_EMPTY() {
            INFO("Contents: " << stream.str());
            REQUIRE(stream.str().empty());
        }
    };

}  // namespace un::log::test
