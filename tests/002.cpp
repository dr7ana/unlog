#include "utils.hpp"

#include <regex>

using namespace un::log::literals;

namespace un::log::test {

    TEST_CASE("002 - formatting uses default pattern markers", "[002][settings][format]") {
        runtime_state_guard guard;
        auto conf = config<>::make();
        auto route = make_channel(conf);
        util::capture_test_logs(conf);

        unlog::info(route, "hello");

        REQUIRE_CONTAINS("info");
        REQUIRE_CONTAINS("hello");
    }

    TEST_CASE("002 - global config", "[002][settings]") {
        runtime_state_guard guard;

        REQUIRE(unlog::get_current_level() == log_level::info);

        set_current_level(log_level::debug);
        REQUIRE(unlog::get_current_level() == log_level::debug);

        set_current_level();
        REQUIRE(unlog::get_current_level() == log_level::info);

        auto conf = config<>::make();
        auto route = make_channel(conf);
        util::capture_test_logs(conf);

        auto global_line = __LINE__ + 1;
        unlog::info(route, "hello from unlog");

        REQUIRE_CONTAINS("hello from unlog");
        REQUIRE_CONTAINS("unlog");
        CHECK_CONTAINS("{}"_format(global_line));
    }

    TEST_CASE("002 - global runtime config", "[002][settings][runtime]") {
        runtime_state_guard guard;
        auto cfg = config<>::make("runtime");

        auto route = make_channel(cfg);

        util::capture_test_logs(cfg);

        unlog::info(route, "runtime path ready");
        REQUIRE_CONTAINS("runtime path ready");
    }

    TEST_CASE("002 - formatting honors compile-time custom pattern with elapsed flag", "[002][settings][format]") {
        runtime_state_guard guard;
        auto conf = config<options::pattern<"[%*] %v">>::make("custom");

        auto route = make_channel(conf);

        util::capture_test_logs(conf, log_level::info);

        unlog::info(route, "hello");
        REQUIRE_CONTAINS("hello");

        auto output = util::captured_output();
        INFO("Contents: " << output);
        CHECK(std::regex_search(output, std::regex(R"(\[\+([0-9]+(?:\.[0-9]+)?)s\]\s+hello)")));
    }

}  // namespace un::log::test
