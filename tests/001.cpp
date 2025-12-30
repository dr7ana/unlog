#include "utils.hpp"

namespace un::log::test {
    TEST_CASE("001 - global settings", "[001]") {
        unlog::info("test case beginning");
        REQUIRE(unlog::get_default_level() == LogLevel::info);

        unlog::info("test underway...");
        unlog::debug("shhh");  // will not print

        set_default_level(LogLevel::debug);
        REQUIRE(unlog::get_default_level() == LogLevel::debug);
        unlog::debug("almost done");

        set_default_level();
        REQUIRE(unlog::get_default_level() == LogLevel::info);
        unlog::info("test case {} complete!", 1);
    }

    TEST_CASE("Config defaults", "[config]") {
        auto cfg = Config::make_default("demo");

        CHECK(cfg.name == "demo");
        CHECK(cfg.cout_log());
        CHECK(cfg.color());
        CHECK_FALSE(cfg.threadsafe());
        CHECK_FALSE(cfg.async());
        CHECK(cfg.threads == 0);
        CHECK(cfg.pool_threads == 0);
        CHECK_FALSE(cfg.filename.has_value());
        CHECK(cfg.file() == std::filesystem::path{"INVALID"});
    }

    TEST_CASE("Config async defaults", "[config]") {
        auto cfg = Config::make_async("async", 2, 4096);

        CHECK(cfg.name == "async");
        CHECK(cfg.cout_log());
        CHECK(cfg.color());
        CHECK(cfg.threadsafe());
        CHECK(cfg.async());
        CHECK(cfg.threads == 2);
        CHECK(cfg.pool_threads == 4096);
    }

    TEST_CASE("Config file defaults", "[config]") {
        auto cfg = Config::make_file(std::filesystem::path{"app.log"}, "file");

        CHECK(cfg.name == "file");
        CHECK(cfg.file_log());
        CHECK(cfg.type == Type::File);
        CHECK_FALSE(cfg.color());
        CHECK(cfg.threadsafe());
        CHECK_FALSE(cfg.async());
        CHECK(cfg.file() == std::filesystem::path{"app.log"});
    }

    TEST_CASE("Config file validation", "[config]") {
        REQUIRE_THROWS_AS((Config{"bad", Type::File, Flags::threadsafe, 0, 0}), std::invalid_argument);
        REQUIRE_THROWS_AS(
                (Config{"bad", Type::cout, Flags::color, 0, 0, std::filesystem::path{"bad.log"}}),
                std::invalid_argument);
        REQUIRE_THROWS_AS(
                (Config{"bad", Type::File, Flags::threadsafe, 0, 0, std::filesystem::path{}}), std::invalid_argument);
    }
}  // namespace un::log::test
