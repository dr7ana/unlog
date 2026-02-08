#include "utils.hpp"

#include <sstream>

namespace un::log::test {

    TEST_CASE("005 - runtime bridge no-op when backend is absent", "[005][backend][bridge]") {
        runtime_state_guard guard;

        bool backend_callback_ran = false;
        get_runtime_backend([&backend_callback_ran]() { backend_callback_ran = true; });

        CHECK_FALSE(backend_callback_ran);
    }

    TEST_CASE("005 - runtime backend bridge returns live backend", "[005][backend][bridge]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("runtime-live-callback");
        make_logger(cfg, true);

        bool callback_ran = false;
        get_runtime_sqpoll_backend([&callback_ran](backend::sqpoll_backend&) { callback_ran = true; });

        CHECK(callback_ran);
    }

    TEST_CASE("005 - rejects mixed sqpoll clock types", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<options::steady>::make_sqpoll("clock-steady");
        make_logger(steady_cfg, true);

        auto system_cfg = config<options::system>::make_sqpoll("clock-system");
        CHECK_THROWS_AS(make_logger(system_cfg, false), std::invalid_argument);
    }

    TEST_CASE("005 - allows multiple sqpoll loggers when clock type matches", "[005][backend][clock]") {
        runtime_state_guard guard;

        auto first_cfg = config<options::steady>::make_sqpoll("same-clock-first");
        make_logger(first_cfg, true);

        auto second_cfg = config<options::steady>::make_sqpoll("same-clock-second");
        CHECK_NOTHROW(make_logger(second_cfg, false));
    }

    TEST_CASE("005 - sqpoll default logger writes to attached sink", "[005][backend][sink]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("default-sqpoll");
        make_logger(cfg, true);

        std::stringstream stream;
        detail::add_sink(cfg, std::make_shared<backend::ostream_sink>(stream));

        unlog::info("sqpoll-default-message");
        unlog::flush();

        CHECK(stream.str().contains("sqpoll-default-message"));
    }

}  // namespace un::log::test
