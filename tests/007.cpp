#include "utils.hpp"

#include <sstream>
#include <string>

namespace un::log::test {

    static bool sqpoll_ready() {
        bool ready = false;
        get_runtime_sqpoll_backend([&ready](backend::sqpoll_backend&) { ready = true; });
        return ready;
    }

    TEST_CASE("007 - global_logger startup initializes sqpoll lane", "[007][runtime][startup]") {
        runtime_state_guard guard;

        auto lg = global_logger();
        REQUIRE(static_cast<bool>(lg));
        CHECK(sqpoll_ready() == true);
    }

    TEST_CASE("007 - default capture helper keeps sqpoll lane active", "[007][runtime][startup]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::info);
        unlog::info("capture-default-message");
        unlog::flush();

        CHECK(sqpoll_ready() == true);
        REQUIRE(util::stream.str().contains("capture-default-message"));
    }

    TEST_CASE("007 - typed add_sink initializes sqpoll lane", "[007][runtime]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("sink-sqpoll-lane");
        std::stringstream stream;
        detail::add_sink(cfg, std::make_shared<backend::ostream_sink>(stream));

        CHECK(sqpoll_ready() == true);
        CHECK(stream.str().empty());
    }

    TEST_CASE("007 - same-lane sqpoll clock mismatch throws", "[007][runtime][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<options::steady>::make_sqpoll("clock-sqpoll-steady");
        make_logger(steady_cfg, true);

        auto system_cfg = config<options::system>::make_sqpoll("clock-sqpoll-system");
        CHECK_THROWS_AS(make_logger(system_cfg, false), std::invalid_argument);
    }

    TEST_CASE("007 - reset_runtime_for_test clears lane", "[007][runtime][reset]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("reset-sqpoll");
        make_logger(cfg, true);

        CHECK(sqpoll_ready() == true);

        reset_runtime_for_test();

        CHECK(sqpoll_ready() == false);
    }

    TEST_CASE("007 - setup remains mutable before first emitted log", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make_sqpoll("pre-activate-first");
        auto second_cfg = config<>::make_sqpoll("pre-activate-second");

        CHECK_NOTHROW(make_logger(first_cfg, true));
        CHECK_NOTHROW(make_logger(second_cfg, false));

        std::stringstream stream;
        CHECK_NOTHROW(detail::add_sink(first_cfg, std::make_shared<backend::ostream_sink>(stream)));
    }

    TEST_CASE("007 - dropped first log does not activate runtime", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("drop-first");
        make_logger(cfg, true);

        auto before = detail::backend_stats();
        auto jumbo = std::string(8192, 'x');
        unlog::info("{}", jumbo);
        auto after_drop = detail::backend_stats();

        CHECK(after_drop.emitted == before.emitted);
        CHECK(after_drop.dropped == (before.dropped + 1u));

        auto pre_activate_cfg = config<>::make_sqpoll("still-open-after-drop");
        CHECK_NOTHROW(make_logger(pre_activate_cfg, false));

        std::stringstream pre_activate_stream;
        CHECK_NOTHROW(detail::add_sink(cfg, std::make_shared<backend::ostream_sink>(pre_activate_stream)));

        unlog::info("activate-runtime");
        auto after_emit = detail::backend_stats();
        CHECK(after_emit.emitted == (after_drop.emitted + 1u));

        auto late_cfg = config<>::make_sqpoll("late-after-emit");
        CHECK_THROWS_AS(make_logger(late_cfg, false), std::invalid_argument);

        std::stringstream late_stream;
        CHECK_THROWS_AS(
                detail::add_sink(cfg, std::make_shared<backend::ostream_sink>(late_stream)), std::invalid_argument);
    }

    TEST_CASE("007 - runtime blocks late setup after first emitted log", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("activate-runtime");
        make_logger(cfg, true);

        unlog::info("activate-runtime");

        auto next_cfg = config<>::make_sqpoll("late-register");
        CHECK_THROWS_AS(make_logger(next_cfg, false), std::invalid_argument);

        std::stringstream stream;
        CHECK_THROWS_AS(detail::add_sink(cfg, std::make_shared<backend::ostream_sink>(stream)), std::invalid_argument);
    }

}  // namespace un::log::test
