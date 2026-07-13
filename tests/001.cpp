#include "utils.hpp"

namespace un::log::test {
    static_assert(decltype(config<>::make())::format_requirements == backend::time_requirements::wall_clock_elapsed);
    static_assert(
            decltype(config<options::pattern<"%v">>::make("plain"))::format_requirements ==
            backend::time_requirements::none);
    static_assert(
            decltype(config<options::pattern<"[%*] %v">>::make("elapsed"))::format_requirements ==
            backend::time_requirements::elapsed);

    TEST_CASE("001 - config tagged option defaults", "[001][config][options]") {
        CHECK((std::same_as<typename config<>::overflow_type, options::drop>));
    }

    TEST_CASE("001 - config tagged option overrides", "[001][config][options]") {
        using override_config = config<options::threadsafe, options::truncate>;

        CHECK((std::same_as<typename override_config::runtime_mode_type, options::threadsafe>));
        CHECK((std::same_as<typename override_config::overflow_type, options::truncate>));

        auto cfg = override_config::make("override");
        CHECK(decltype(cfg)::runtime_mode == RuntimeMode::threadsafe);
    }

    TEST_CASE("001 - global config defaults", "[001][config][global]") {
        CHECK(test_log::config == default_global_config);
    }

    TEST_CASE("001 - runtime queue capacity derives from global thread buffer size", "[001][config][global]") {
        CHECK(backend::configured_queue_traits<test_log::config, false>::capacity == 1024u);
    }

    TEST_CASE("001 - config defaults", "[001][config]") {
        auto cfg = config<>::make("demo");

        CHECK(cfg.name == "demo");
        CHECK_FALSE(cfg.color());
        CHECK(cfg.sink_type == SinkType::cout);
        CHECK(detail::config_sink_type(cfg) == SinkType::cout);
        CHECK(detail::config_overflow_policy_v<decltype(cfg)> == OverflowPolicy::drop);
        CHECK(detail::config_pattern_program(cfg).text == DEFAULT_PATTERN);
        CHECK_FALSE(cfg.filename.has_value());
        CHECK_FALSE(cfg.output_fd.has_value());
        CHECK_FALSE(cfg.unix_dgram_path.has_value());
    }

    TEST_CASE("001 - config color remains explicit opt-in", "[001][config]") {
        auto cfg = test_helper::make_config("color", SinkType::cout, Flags::color);

        CHECK(cfg.color());
        CHECK(detail::config_pattern_program(cfg).text == DEFAULT_PATTERN_COLOR);
    }

    TEST_CASE("001 - stderr config defaults to plain output", "[001][config]") {
        auto cfg = config<>::make_stderr("stderr");

        CHECK_FALSE(cfg.color());
        CHECK(detail::config_pattern_program(cfg).text == DEFAULT_PATTERN);
    }

    TEST_CASE("001 - config file defaults", "[001][config]") {
        auto cfg = config<>::make_file(fs::path{"app.log"}, "file");

        CHECK(cfg.name == "file");
        CHECK_FALSE(cfg.color());
        CHECK(detail::config_pattern_program(cfg).text == DEFAULT_PATTERN);
        CHECK(cfg.filename == fs::path{"app.log"});
        CHECK(detail::config_sink_type(cfg) == SinkType::file);
        REQUIRE(detail::config_filename(cfg).has_value());
        CHECK(detail::config_filename(cfg).value() == fs::path{"app.log"});
    }

    TEST_CASE("001 - config selects a statically compiled custom pattern", "[001][config][format]") {
        auto cfg = config<options::pattern<"[%*] %v">>::make("custom-format");
        auto program = detail::config_pattern_program(cfg);

        CHECK(cfg.name == "custom-format");
        CHECK(program.text == "[%*] %v");
        CHECK(program.pieces.data() == decltype(cfg)::pattern_type::compiled.data());
        CHECK_FALSE(program.pieces.empty());
    }

    TEST_CASE("001 - config file validation", "[001][config]") {
        REQUIRE_THROWS_AS((config<>::make_file(fs::path{}, "bad")), std::invalid_argument);
        REQUIRE_THROWS_AS((test_helper::make_config("bad", SinkType::file, 0)), std::invalid_argument);
    }

    TEST_CASE("001 - config fd sink validation", "[001][config]") {
        REQUIRE_THROWS_AS((test_helper::make_config("bad-fd", SinkType::fd, Flags::color)), std::invalid_argument);

        auto cfg = test_helper::make_config("fd-sink", SinkType::fd, 0, std::nullopt, 2);
        CHECK(cfg.sink_type == SinkType::fd);
        REQUIRE(cfg.output_fd.has_value());
        CHECK(cfg.output_fd.value() == 2);
        CHECK(detail::config_sink_type(cfg) == SinkType::fd);
    }

    TEST_CASE("001 - config unix dgram sink validation", "[001][config]") {
        REQUIRE_THROWS_AS(
                (test_helper::make_config("bad-dgram", SinkType::unix_dgram, Flags::color)), std::invalid_argument);

        auto cfg = test_helper::make_config(
                "dgram-sink", SinkType::unix_dgram, 0, std::nullopt, std::nullopt, fs::path{"/tmp/unlog-test.sock"});
        CHECK(cfg.sink_type == SinkType::unix_dgram);
        REQUIRE(cfg.unix_dgram_path.has_value());
        CHECK(cfg.unix_dgram_path.value() == fs::path{"/tmp/unlog-test.sock"});
        CHECK(detail::config_sink_type(cfg) == SinkType::unix_dgram);
    }
}  // namespace un::log::test
