#include "utils.hpp"

#include <string>

namespace un::log::test {

    TEST_CASE("004 - default channel path emits through runtime queue", "[004][runtime][channel]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("hotpotato");
        make_channel(cfg, true);
        util::capture_test_logs(cfg);

        unlog::info("runtime-message");

        REQUIRE_CONTAINS("runtime-message");
    }

#if UNLOG_DIAGNOSTIC
    TEST_CASE("004 - backend stats move on emitted log", "[004][backend][stats]") {
        runtime_state_guard guard;

        util::capture_test_logs();

        auto before = un::log::detail::backend_stats();
        unlog::info("stats-check-{}", 7);
        auto after = un::log::detail::backend_stats();

        CHECK(after.emitted == (before.emitted + 1u));
    }

    TEST_CASE("004 - producer counters drop oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("counter-drop-oversize");

        make_channel(cfg, true);
        util::capture_test_logs(cfg);

        auto before = un::log::detail::backend_stats();
        auto msg = std::string{"oversize-drop-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8206u);

        unlog::info("{}", msg);

        auto after = un::log::detail::backend_stats();
        CHECK(after.emitted == before.emitted);
        CHECK(after.dropped == (before.dropped + 1u));
        CHECK(after.truncated == before.truncated);
        CHECK_EMPTY{};
    }

    TEST_CASE("004 - producer counters truncate oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<options::truncate>::make("counter-truncate-oversize");

        auto route = make_channel(cfg, false);
        util::capture_test_logs(cfg);

        auto before = un::log::detail::backend_stats();
        auto msg = std::string{"oversize-truncate-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8210u);

        unlog::info(route, "{}", msg);

        auto after = un::log::detail::backend_stats();
        CHECK(after.emitted == (before.emitted + 1u));
        CHECK(after.dropped == before.dropped);
        CHECK(after.truncated == (before.truncated + 1u));
        REQUIRE_CONTAINS("oversize-truncate-");
        CHECK_NOT_CONTAINS{msg};
    }

    TEST_CASE("004 - channel max_record_size tightens truncate budget", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<options::truncate, options::max_record_size<256>>::make("small-truncate-limit");

        auto route = make_channel(cfg, false);
        util::capture_test_logs(cfg);

        auto max_message_size = backend::max_message_size_for_runtime_record_limit(decltype(cfg)::max_record_size);
        REQUIRE(max_message_size.has_value());

        auto before = un::log::detail::backend_stats();
        auto msg = std::string{"small-limit-"} + std::string(*max_message_size + 32u, 'x');

        unlog::info(route, "{}", msg);

        auto after = un::log::detail::backend_stats();
        CHECK(after.emitted == (before.emitted + 1u));
        CHECK(after.dropped == before.dropped);
        CHECK(after.truncated == (before.truncated + 1u));
        REQUIRE_CONTAINS{"small-limit-"};
        CHECK_NOT_CONTAINS{msg};
    }
#endif

}  // namespace un::log::test
