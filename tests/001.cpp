#include "utils.hpp"

namespace un::log::test {
    TEST_CASE("001 - config defaults", "[001][config]") {
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

    TEST_CASE("001 - config async defaults", "[001][config]") {
        auto cfg = Config::make_async("async", 2, 4096);

        CHECK(cfg.name == "async");
        CHECK(cfg.cout_log());
        CHECK(cfg.color());
        CHECK(cfg.threadsafe());
        CHECK(cfg.async());
        CHECK(cfg.threads == 2);
        CHECK(cfg.pool_threads == 4096);
    }

    TEST_CASE("001 - config file defaults", "[001][config]") {
        auto cfg = Config::make_file(std::filesystem::path{"app.log"}, "file");

        CHECK(cfg.name == "file");
        CHECK(cfg.file_log());
        CHECK(cfg.type == Type::File);
        CHECK_FALSE(cfg.color());
        CHECK(cfg.threadsafe());
        CHECK_FALSE(cfg.async());
        CHECK(cfg.file() == std::filesystem::path{"app.log"});
    }

    TEST_CASE("001 - config file validation", "[001][config]") {
        REQUIRE_THROWS_AS((Config{"bad", Type::File, Flags::threadsafe, 0, 0}), std::invalid_argument);
        REQUIRE_THROWS_AS(
                (Config{"bad", std::filesystem::path{"bad.log"}, Type::cout, Flags::color, 0, 0}),
                std::invalid_argument);
        REQUIRE_THROWS_AS(
                (Config{"bad", std::filesystem::path{}, Type::File, Flags::threadsafe, 0, 0}), std::invalid_argument);
    }
}  // namespace un::log::test
