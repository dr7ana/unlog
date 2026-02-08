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
        CHECK(cfg.file() == fs::path{"INVALID"});

        CHECK(cfg.backend.memory_policy == MemoryPolicy::sqpoll_live);
        CHECK(cfg.backend.overflow_policy == OverflowPolicy::drop);
        CHECK(cfg.backend.timestamp_mode == ClockType::steady);
        CHECK(cfg.backend.thread_bufsize == (1u << 20));
        CHECK(cfg.backend.max_record_size == 4096);
        CHECK(cfg.backend.strict_nonblocking);
        CHECK(cfg.backend.sqpoll_queue_depth == 4096);
        CHECK_FALSE(cfg.backend.output_fd.has_value());
        CHECK_FALSE(cfg.backend.unix_dgram_path.has_value());
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
        auto cfg = Config::make_file(fs::path{"app.log"}, "file");

        CHECK(cfg.name == "file");
        CHECK(cfg.file_log());
        CHECK(cfg.type == SinkType::file);
        CHECK_FALSE(cfg.color());
        CHECK(cfg.threadsafe());
        CHECK_FALSE(cfg.async());
        CHECK(cfg.file() == fs::path{"app.log"});
    }

    TEST_CASE("001 - config file validation", "[001][config]") {
        REQUIRE_THROWS_AS((Config{"bad", SinkType::file, Flags::threadsafe, 0, 0}), std::invalid_argument);
        REQUIRE_THROWS_AS(
                (Config{"bad", fs::path{"bad.log"}, SinkType::cout, Flags::color, 0, 0}), std::invalid_argument);
        REQUIRE_THROWS_AS((Config{"bad", fs::path{}, SinkType::file, Flags::threadsafe, 0, 0}), std::invalid_argument);
    }

    TEST_CASE("001 - config fd sink validation", "[001][config]") {
        REQUIRE_THROWS_AS((Config{"bad-fd", SinkType::fd, Flags::color, 0, 0}), std::invalid_argument);

        auto backend = BackendOptions{};
        backend.output_fd = 2;

        auto cfg = Config{"fd-sink", SinkType::fd, Flags::threadsafe, 0, 0, std::nullopt, backend};
        CHECK(cfg.fd_log());
        CHECK(cfg.type == SinkType::fd);
        REQUIRE(cfg.backend.output_fd.has_value());
        CHECK(cfg.backend.output_fd.value() == 2);
        CHECK(cfg.to_string().find("type=fd") != std::string::npos);
    }

    TEST_CASE("001 - config unix dgram sink validation", "[001][config]") {
        REQUIRE_THROWS_AS((Config{"bad-dgram", SinkType::unix_dgram, Flags::color, 0, 0}), std::invalid_argument);

        auto backend = BackendOptions{};
        backend.unix_dgram_path = fs::path{"/tmp/unlog-test.sock"};

        auto cfg = Config{"dgram-sink", SinkType::unix_dgram, Flags::threadsafe, 0, 0, std::nullopt, backend};
        CHECK(cfg.unix_dgram_log());
        CHECK(cfg.type == SinkType::unix_dgram);
        REQUIRE(cfg.backend.unix_dgram_path.has_value());
        CHECK(cfg.backend.unix_dgram_path.value() == fs::path{"/tmp/unlog-test.sock"});
        CHECK(cfg.to_string().find("type=unix_dgram") != std::string::npos);
    }
}  // namespace un::log::test
