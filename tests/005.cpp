#include "utils.hpp"

#include <sstream>
#include <thread>

namespace un::log::test {

    TEST_CASE("005 - runtime bridge no-op when backend is absent", "[005][backend][bridge]") {
        runtime_state_guard guard;

        bool backend_callback_ran = false;
        get_runtime_backend([&backend_callback_ran]() { backend_callback_ran = true; });

        CHECK_FALSE(backend_callback_ran);
    }

    TEST_CASE("005 - runtime backend bridge returns live backend", "[005][backend][bridge]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("runtime-live-callback");
        test_log::make_channel(cfg);
        test_log::start();

        bool callback_ran = false;
        get_runtime_backend([&callback_ran] { callback_ran = true; });

        CHECK(callback_ran);
    }

    TEST_CASE("005 - rejects mixed runtime clock types", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<>::make("clock-steady");
        test_log::make_channel(steady_cfg);

        constexpr auto system_config = global_config{.clock_type = ClockType::system};
        using system_log = configured<system_config>;
        auto system_cfg = config<>::make("clock-system");
        CHECK_THROWS_AS(system_log::make_channel(system_cfg), std::invalid_argument);
    }

    TEST_CASE("005 - allows multiple runtime channels when clock type matches", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make("same-clock-first");
        test_log::make_channel(first_cfg);

        auto second_cfg = config<>::make("same-clock-second");
        CHECK_NOTHROW(test_log::make_channel(second_cfg));
    }

    TEST_CASE("005 - allows mixed runtime modes", "[005][backend][mode]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make("single-threaded");
        test_log::make_channel(first_cfg);

        auto second_cfg = config<options::threadsafe>::make("threadsafe");
        CHECK_NOTHROW(test_log::make_channel(second_cfg));
    }

    TEST_CASE("005 - explicit channel writes to attached sink", "[005][backend][sink]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("explicit-runtime");
        auto route = test_log::make_channel(cfg);

        std::stringstream stream;
        detail::add_sink<test_log::config>(cfg, std::make_shared<backend::ostream_sink_sc>(stream));
        test_log::start();

        unlog::info(route, "runtime-explicit-message");
        test_log::flush();

        CHECK(stream.str().contains("runtime-explicit-message"));
    }

    TEST_CASE("005 - explicit threadsafe mode supports multi-threaded logging", "[005][backend][mode]") {
        runtime_state_guard guard;

        auto cfg = config<options::threadsafe>::make("threadsafe-runtime");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg);
        test_log::start();

        std::jthread worker{[route] { unlog::info(route, "threadsafe-worker-message"); }};
        unlog::info(route, "threadsafe-main-message");
        worker.join();

        test_log::flush();

        REQUIRE_CONTAINS{"threadsafe-worker-message"};
        REQUIRE_CONTAINS{"threadsafe-main-message"};
    }

}  // namespace un::log::test
