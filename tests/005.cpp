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
        make_channel(cfg, true);

        bool callback_ran = false;
        get_runtime_backend([&callback_ran] { callback_ran = true; });

        CHECK(callback_ran);
    }

    TEST_CASE("005 - rejects mixed runtime clock types", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<options::steady>::make("clock-steady");
        make_channel(steady_cfg, true);

        auto system_cfg = config<options::system>::make("clock-system");
        CHECK_THROWS_AS(make_channel(system_cfg, false), std::invalid_argument);
    }

    TEST_CASE("005 - allows multiple runtime channels when clock type matches", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto first_cfg = config<options::steady>::make("same-clock-first");
        make_channel(first_cfg, true);

        auto second_cfg = config<options::steady>::make("same-clock-second");
        CHECK_NOTHROW(make_channel(second_cfg, false));
    }

    TEST_CASE("005 - rejects mixed runtime modes", "[005][backend][mode]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make("single-threaded");
        make_channel(first_cfg, true);

        CHECK_THROWS_AS(
                set_global_config(global_config{.mode = RuntimeMode::threadsafe, .thread_bufsize = 1 << 20}),
                std::invalid_argument);
    }

    TEST_CASE("005 - default channel writes to attached sink", "[005][backend][sink]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("default-runtime");
        make_channel(cfg, true);

        std::stringstream stream;
        detail::add_sink(cfg, std::make_shared<backend::ostream_sink_sc>(stream));

        unlog::info("runtime-default-message");
        unlog::flush();

        CHECK(stream.str().contains("runtime-default-message"));
    }

    TEST_CASE("005 - explicit threadsafe mode supports multi-threaded logging", "[005][backend][mode]") {
        runtime_state_guard guard;

        set_global_config(global_config{.mode = RuntimeMode::threadsafe, .thread_bufsize = 1 << 20});

        auto cfg = config<>::make("threadsafe-runtime");
        auto route = make_channel(cfg, true);
        util::capture_test_logs(cfg);

        std::jthread worker{[route] { unlog::info(route, "threadsafe-worker-message"); }};
        unlog::info(route, "threadsafe-main-message");
        worker.join();

        unlog::flush();

        REQUIRE_CONTAINS{"threadsafe-worker-message"};
        REQUIRE_CONTAINS{"threadsafe-main-message"};
    }

}  // namespace un::log::test
