#include "utils.hpp"

#include <regex>

using namespace un::log::literals;

namespace un::log::test {

    TEST_CASE("002 - formatting uses default pattern markers", "[002][settings][format]") {
        runtime_state_guard guard;
        auto conf = config<>::make_sqpoll();
        make_logger(conf, true);
        util::capture_test_logs(conf);

        unlog::info("hello");

        REQUIRE_CONTAINS("info");
        REQUIRE_CONTAINS("hello");
    }

    TEST_CASE("002 - global config", "[002][settings]") {
        runtime_state_guard guard;

        REQUIRE(unlog::get_default_level() == log_level::info);

        set_default_level(log_level::debug);
        REQUIRE(unlog::get_default_level() == log_level::debug);

        set_default_level();
        REQUIRE(unlog::get_default_level() == log_level::info);

        auto conf = config<>::make_sqpoll();
        make_logger(conf, true);
        util::capture_test_logs(conf);

        auto global_line = __LINE__ + 1;
        unlog::info("hello from unlog");

        REQUIRE_CONTAINS("hello from unlog");
        REQUIRE_CONTAINS("unlog");
        CHECK_CONTAINS("{}"_format(global_line));
    }

    TEST_CASE("002 - global sqpoll config", "[002][settings][sqpoll]") {
        runtime_state_guard guard;
        auto cfg = config<>::make_sqpoll("sqpoll");

        make_logger(cfg, true);

        util::capture_test_logs(cfg);

        unlog::info("sqpoll path ready");
        REQUIRE_CONTAINS("sqpoll path ready");
    }

    TEST_CASE("002 - formatting honors custom pattern with elapsed flag", "[002][settings][format]") {
        runtime_state_guard guard;
        auto conf = test_helper::make_sqpoll_config("custom", SinkType::cout, Flags::color, "[%*] %v");

        make_logger(conf, true);

        util::capture_test_logs(conf, log_level::info);

        unlog::info("hello");
        REQUIRE_CONTAINS("hello");

        auto output = util::stream.str();
        INFO("Contents: " << output);
        CHECK(std::regex_search(output, std::regex(R"(\[\+([0-9]+(?:\.[0-9]+)?)s\]\s+hello)")));
    }

}  // namespace un::log::test
