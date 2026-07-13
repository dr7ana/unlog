#include "utils.hpp"

#include <exception>
#include <future>
#include <sstream>
#include <string>
#include <thread>

namespace un::log::test {

    inline constexpr auto large_global_config = global_config{.thread_bufsize = size_t{1} << 22};
    using large_test_log = configured<large_global_config>;
    inline constexpr auto churn_global_config = global_config{.max_producers = 1u};
    using churn_test_log = configured<churn_global_config>;

    static bool runtime_ready() {
        bool ready = false;
        get_runtime_backend([&ready] { ready = true; });
        return ready;
    }

    TEST_CASE("007 - explicit capture helper keeps runtime active", "[007][runtime][startup]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("capture-explicit");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg, log_level::info);
        test_log::start();
        unlog::info(route, "capture-explicit-message");
        test_log::flush();

        CHECK(runtime_ready() == true);
        REQUIRE_CONTAINS{"capture-explicit-message"};
    }

    TEST_CASE("007 - typed add_sink remains configuration-only before start", "[007][runtime]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("sink-runtime");
        std::stringstream stream;
        detail::add_sink<test_log::config>(cfg, std::make_shared<backend::ostream_sink_sc>(stream));

        CHECK(runtime_ready() == false);
        CHECK(consumer_thread_started() == false);
        CHECK(stream.str().empty());
    }

    TEST_CASE("007 - make_channel returns routable handle", "[007][runtime][channel]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::info);

        auto cfg = config<>::make("channel-route");
        auto route = test_log::make_channel(cfg);
        REQUIRE(static_cast<bool>(route));
        CHECK(route.name() == "channel-route"sv);
        CHECK(consumer_thread_started() == false);

        test_log::start();
        CHECK(consumer_thread_started() == true);

        unlog::info(route, "channel-message");
        test_log::flush();

        CHECK(runtime_ready() == true);
        REQUIRE_CONTAINS{"channel-message"};
    }

    TEST_CASE("007 - channel set_level gates channel logging", "[007][runtime][channel][level]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::trace);

        auto cfg = config<>::make("channel-level");
        auto route = test_log::make_channel(cfg);
        test_log::start();
        route.set_level(log_level::err);

        unlog::info(route, "hidden-channel-message");
        test_log::flush();
        REQUIRE_EMPTY{};

        unlog::error(route, "visible-channel-message");
        test_log::flush();
        REQUIRE_CONTAINS{"visible-channel-message"};
    }

    TEST_CASE("007 - pre-start channel level remains inert until activation", "[007][runtime][channel][level]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("pre-start-level");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(log_level::trace);
        route.set_level(log_level::err);

        unlog::error(route, "before-start-hidden");
        test_log::start();
        unlog::info(route, "below-route-level");
        unlog::error(route, "after-start-visible");

        auto output = util::captured_output();
        CHECK_FALSE(output.contains("before-start-hidden"));
        CHECK_FALSE(output.contains("below-route-level"));
        CHECK(output.contains("after-start-visible"));
    }

    TEST_CASE("007 - runtime clock mismatch throws", "[007][runtime][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<>::make("clock-steady");
        test_log::make_channel(steady_cfg);

        constexpr auto system_config = global_config{.clock_type = ClockType::system};
        using system_log = configured<system_config>;
        auto system_cfg = config<>::make("clock-system");
        CHECK_THROWS_AS(system_log::make_channel(system_cfg), std::invalid_argument);
    }

    TEST_CASE("007 - global config applies before first channel", "[007][runtime][global-config]") {
        runtime_state_guard guard;

        auto cfg = config<options::threadsafe>::make("global-config-threadsafe");
        large_test_log::make_channel(cfg);
        large_test_log::start();

        CHECK_NOTHROW(large_test_log::prewarm_thread());
        CHECK(large_test_log::config.thread_bufsize == (size_t{1} << 22));
        CHECK(backend::configured_queue_traits<large_global_config, false>::capacity == 4096u);
    }

    TEST_CASE("007 - prewarm_thread is a no-op without runtime", "[007][runtime][startup]") {
        runtime_state_guard guard;

        CHECK_NOTHROW(test_log::prewarm_thread());
        CHECK(runtime_ready() == false);
    }

    TEST_CASE("007 - prewarm_thread can register a worker producer before first log", "[007][runtime][prewarm]") {
        runtime_state_guard guard;

        auto cfg = config<options::threadsafe>::make("prewarm-worker");
        auto route = test_log::make_channel(cfg);
        util::capture_test_logs(cfg, log_level::info);
        test_log::start();

        REQUIRE(test_helper::producer_count() == 0u);

        SECTION("vanilla") {
            std::promise<void> ready_promise;
            auto ready = ready_promise.get_future();
            std::promise<void> go_promise;
            auto go = go_promise.get_future().share();

            auto worker = std::jthread([&](std::stop_token) {
                ready_promise.set_value();
                go.wait();
                unlog::info(route, "vanilla-worker-message");
            });

            ready.wait();
            CHECK(test_helper::producer_count() == 0u);

            go_promise.set_value();
            worker.join();

            test_log::flush();
            CHECK(test_helper::producer_count() == 1u);
            REQUIRE_CONTAINS{"vanilla-worker-message"};
        }

        SECTION("prewarm") {
            std::promise<void> ready_promise;
            auto ready = ready_promise.get_future();
            std::promise<void> go_promise;
            auto go = go_promise.get_future().share();

            auto worker = std::jthread([&](std::stop_token) {
                test_log::prewarm_thread();
                ready_promise.set_value();
                go.wait();
                unlog::info(route, "prewarm-worker-message");
            });

            ready.wait();
            CHECK(test_helper::producer_count() == 1u);

            go_promise.set_value();
            worker.join();

            test_log::flush();
            CHECK(test_helper::producer_count() == 1u);
            REQUIRE_CONTAINS{"prewarm-worker-message"};
        }
    }

    TEST_CASE("007 - retired producer slots are reused across thread churn", "[007][runtime][producer][churn]") {
        runtime_state_guard guard;

        auto cfg = config<options::threadsafe>::make("producer-churn");
        auto route = churn_test_log::make_channel(cfg);
        util::capture_test_logs<churn_global_config>(cfg, log_level::info);
        churn_test_log::start();

        for (int i = 0; i < 8; ++i) {
            auto worker = std::thread{[&, i] { unlog::info(route, "churn-{}", i); }};
            worker.join();
            churn_test_log::flush();
        }

        CHECK(test_helper::producer_count() == 1u);
        auto output = util::captured_output<churn_global_config>();
        for (int i = 0; i < 8; ++i) {
            CHECK(output.contains("churn-{}"_format(i)));
        }
    }

    TEST_CASE("007 - reset_runtime_for_test clears runtime", "[007][runtime][reset]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("reset-runtime");
        test_log::make_channel(cfg);
        test_log::start();

        CHECK(runtime_ready() == true);
        CHECK(consumer_thread_started() == true);

        reset_runtime_for_test();

        CHECK(runtime_ready() == false);
        CHECK(consumer_thread_started() == false);
        CHECK(test_log::config.thread_bufsize == options::default_thread_bufsize);
    }

    TEST_CASE("007 - setup remains mutable before explicit start", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make("pre-activate-first");
        auto second_cfg = config<>::make("pre-activate-second");

        CHECK_NOTHROW(test_log::make_channel(first_cfg));
        CHECK_NOTHROW(test_log::make_channel(second_cfg));

        std::stringstream stream;
        CHECK_NOTHROW(
                detail::add_sink<test_log::config>(first_cfg, std::make_shared<backend::ostream_sink_sc>(stream)));
    }

#if UNLOG_DIAGNOSTIC
    TEST_CASE("007 - logging before start is inert and does not seal configuration", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("drop-first");
        auto route = test_log::make_channel(cfg);

        auto before = test_log::stats();
        auto jumbo = std::string(8192, 'x');
        unlog::info(route, "{}", jumbo);
        auto after_drop = test_log::stats();

        CHECK(after_drop.emitted == before.emitted);
        CHECK(after_drop.dropped == before.dropped);

        auto pre_activate_cfg = config<>::make("still-open-after-drop");
        CHECK_NOTHROW(test_log::make_channel(pre_activate_cfg));

        std::stringstream pre_activate_stream;
        CHECK_NOTHROW(
                detail::add_sink<test_log::config>(
                        cfg, std::make_shared<backend::ostream_sink_sc>(pre_activate_stream)));

        test_log::start();
        unlog::info(route, "activate-runtime");
        auto after_emit = test_log::stats();
        CHECK(after_emit.emitted == (after_drop.emitted + 1u));

        auto late_cfg = config<>::make("late-after-emit");
        CHECK_THROWS_AS(test_log::make_channel(late_cfg), std::invalid_argument);

        std::stringstream late_stream;
        CHECK_THROWS_AS(
                detail::add_sink<test_log::config>(cfg, std::make_shared<backend::ostream_sink_sc>(late_stream)),
                std::invalid_argument);
    }

    TEST_CASE("007 - single-thread ownership binds on the first enabled log", "[007][runtime][diagnostic]") {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        auto route = test_log::make_channel(config<>::make_fd(null_sink.get(), "single-thread-owner"));
        test_log::start();

        unlog::trace(route, "disabled calls do not bind ownership");

        auto owner_error = std::exception_ptr{};
        auto owner = std::thread{[&] {
            try {
                unlog::info(route, "owner thread binds here");
            } catch (...) {
                owner_error = std::current_exception();
            }
        }};
        owner.join();

        REQUIRE(owner_error == nullptr);
        CHECK_THROWS_AS(unlog::info(route, "a second thread is rejected"), std::logic_error);
    }
#endif

    TEST_CASE("007 - runtime blocks late setup after explicit start", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("activate-runtime");
        test_log::make_channel(cfg);

        test_log::start();

        auto next_cfg = config<>::make("late-register");
        CHECK_THROWS_AS(test_log::make_channel(next_cfg), std::invalid_argument);

        std::stringstream stream;
        CHECK_THROWS_AS(
                detail::add_sink<test_log::config>(cfg, std::make_shared<backend::ostream_sink_sc>(stream)),
                std::invalid_argument);
    }

    TEST_CASE("007 - process rejects a second compile-time global config", "[007][runtime][global-config]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("global-config-owner");
        test_log::make_channel(cfg);

        constexpr auto other_config = global_config{.thread_bufsize = size_t{1} << 21};
        using other_log = configured<other_config>;
        auto other_cfg = config<>::make("other-global-config");
        CHECK_THROWS_AS(other_log::make_channel(other_cfg), std::invalid_argument);
    }

}  // namespace un::log::test
