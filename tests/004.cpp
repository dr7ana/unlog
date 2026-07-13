#include "utils.hpp"

#include <string>

namespace un::log::test {

    TEST_CASE("004 - explicit channel path emits through runtime queue", "[004][runtime][channel]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("hotpotato");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg);
        test_log::start();

        unlog::info(route, "runtime-message");

        REQUIRE_CONTAINS("runtime-message");
    }

#if UNLOG_DIAGNOSTIC
    TEST_CASE("004 - backend stats move on emitted log", "[004][backend][stats]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("stats");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg);
        test_log::start();

        auto before = test_log::stats();
        unlog::info(route, "stats-check-{}", 7);
        auto after = test_log::stats();

        CHECK(after.emitted == (before.emitted + 1u));
    }

    TEST_CASE("004 - producer counters drop oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("counter-drop-oversize");

        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg);
        test_log::start();

        auto before = test_log::stats();
        auto msg = std::string{"oversize-drop-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8206u);

        unlog::info(route, "{}", msg);

        auto after = test_log::stats();
        CHECK(after.emitted == before.emitted);
        CHECK(after.dropped == (before.dropped + 1u));
        CHECK(after.truncated == before.truncated);
        CHECK_EMPTY{};
    }

    TEST_CASE("004 - producer counters truncate oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<options::truncate>::make("counter-truncate-oversize");

        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg);
        test_log::start();

        auto before = test_log::stats();
        auto msg = std::string{"oversize-truncate-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8210u);

        unlog::info(route, "{}", msg);

        auto after = test_log::stats();
        CHECK(after.emitted == (before.emitted + 1u));
        CHECK(after.dropped == before.dropped);
        CHECK(after.truncated == (before.truncated + 1u));
        REQUIRE_CONTAINS("oversize-truncate-");
        CHECK_NOT_CONTAINS{msg};
    }

#endif

}  // namespace un::log::test
