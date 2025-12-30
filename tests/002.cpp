#include "utils.hpp"

#include <regex>

using namespace un::log::literals;

namespace un::log::test {

    TEST_CASE("002 - formatting uses default pattern markers", "[002][settings][format]") {
        util::capture_test_logs();

        unlog::info("hello");

        util::REQUIRE_CONTAINS("[unlog:info|");
        util::REQUIRE_CONTAINS(">> hello");
    }

    TEST_CASE("002 - global config", "[002][settings]") {
        util::capture_test_logs();

        REQUIRE(unlog::get_default_level() == LogLevel::info);

        set_default_level(LogLevel::debug);
        REQUIRE(unlog::get_default_level() == LogLevel::debug);

        set_default_level();
        REQUIRE(unlog::get_default_level() == LogLevel::info);

        auto global_line = __LINE__ + 1;
        unlog::info("hello from unlog");

        util::REQUIRE_CONTAINS("hello from unlog");
        util::REQUIRE_CONTAINS("unlog");
        util::CHECK_CONTAINS("{}"_format(global_line));

        auto cfg = Config::make_async("async", 3, 1024);
        CHECK(cfg.async());
        CHECK(cfg.threadsafe());

        make_logger(cfg, false);

        unlog::info("async path ready");
        util::REQUIRE_CONTAINS("async path ready");
    }

    TEST_CASE("002 - formatting honors custom pattern with elapsed flag", "[002][settings][format]") {
        auto format = std::string{"[%*] %v"};
        auto conf = Config{"custom", Type::cout, Flags::color, 0, 0, format};

        make_logger(conf, true);

        util::capture_test_logs(conf, LogLevel::info);

        unlog::info("hello");
        util::REQUIRE_CONTAINS("[+0.000s] hello");

        auto output = util::stream.str();
        INFO("Contents: " << output);
        CHECK(std::regex_search(output, std::regex(R"(\[\+([0-9]+(?:\.[0-9]+)?)s\]\s+hello)")));
    }

}  // namespace un::log::test
