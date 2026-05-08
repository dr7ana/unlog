#include "utils.hpp"

#include <future>
#include <sstream>
#include <string>
#include <thread>

namespace un::log::test {

    static bool runtime_ready() {
        bool ready = false;
        get_runtime_backend([&ready] { ready = true; });
        return ready;
    }

    TEST_CASE("007 - explicit capture helper keeps runtime active", "[007][runtime][startup]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("capture-explicit");
        auto route = make_channel(cfg);
        util::capture_test_logs(cfg, log_level::info);
        unlog::info(route, "capture-explicit-message");
        unlog::flush();

        CHECK(runtime_ready() == true);
        REQUIRE_CONTAINS{"capture-explicit-message"};
    }

    TEST_CASE("007 - typed add_sink initializes runtime", "[007][runtime]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("sink-runtime");
        std::stringstream stream;
        detail::add_sink(cfg, std::make_shared<backend::ostream_sink_sc>(stream));

        CHECK(runtime_ready() == true);
        CHECK(stream.str().empty());
    }

    TEST_CASE("007 - make_channel returns routable handle", "[007][runtime][channel]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::info);

        auto cfg = config<>::make("channel-route");
        auto route = make_channel(cfg);
        REQUIRE(static_cast<bool>(route));
        CHECK(route.name() == "channel-route"sv);
        CHECK(consumer_thread_started() == true);

        unlog::info(route, "channel-message");
        unlog::flush();

        CHECK(runtime_ready() == true);
        REQUIRE_CONTAINS{"channel-message"};
    }

    TEST_CASE("007 - channel set_level gates channel logging", "[007][runtime][channel][level]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::trace);

        auto cfg = config<>::make("channel-level");
        auto route = make_channel(cfg);
        route.set_level(log_level::err);

        unlog::info(route, "hidden-channel-message");
        unlog::flush();
        REQUIRE_EMPTY{};

        unlog::error(route, "visible-channel-message");
        unlog::flush();
        REQUIRE_CONTAINS{"visible-channel-message"};
    }

    TEST_CASE("007 - runtime clock mismatch throws", "[007][runtime][clock]") {
        runtime_state_guard guard;

        auto steady_cfg = config<options::steady>::make("clock-steady");
        make_channel(steady_cfg);

        auto system_cfg = config<options::system>::make("clock-system");
        CHECK_THROWS_AS(make_channel(system_cfg), std::invalid_argument);
    }

    TEST_CASE("007 - global config applies before first channel", "[007][runtime][global-config]") {
        runtime_state_guard guard;

        constexpr auto thread_bufsize = size_t{1} << 22;
        auto expected_capacity = backend::runtime_queue_traits<false>::queue_capacity_for(thread_bufsize);
        REQUIRE(expected_capacity.has_value());

        set_global_config(global_config{.thread_bufsize = thread_bufsize});

        auto cfg = config<options::threadsafe>::make("global-config-threadsafe");
        make_channel(cfg);

        CHECK_NOTHROW(prewarm_thread());
        using policy = detail::channel_policy_for<decltype(cfg)>;
        auto& producer = detail::get_runtime_queue_producer<policy>();
        CHECK(get_global_config().thread_bufsize == thread_bufsize);
        CHECK(producer.queue().capacity() == *expected_capacity);
    }

    TEST_CASE("007 - prewarm_thread is a no-op without runtime", "[007][runtime][startup]") {
        runtime_state_guard guard;

        CHECK_NOTHROW(prewarm_thread());
        CHECK(runtime_ready() == false);
    }

    TEST_CASE("007 - prewarm_thread can register a worker producer before first log", "[007][runtime][prewarm]") {
        runtime_state_guard guard;

        constexpr auto thread_bufsize = size_t{1} << 22;
        set_global_config(global_config{.thread_bufsize = thread_bufsize});
        auto cfg = config<options::threadsafe>::make("prewarm-worker");
        auto route = make_channel(cfg);
        util::capture_test_logs(cfg, log_level::info);

        REQUIRE(test_helper::producer_count() == 1u);

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
            CHECK(test_helper::producer_count() == 1u);

            go_promise.set_value();
            worker.join();

            unlog::flush();
            CHECK(test_helper::producer_count() == 2u);
            REQUIRE_CONTAINS{"vanilla-worker-message"};
        }

        SECTION("prewarm") {
            std::promise<void> ready_promise;
            auto ready = ready_promise.get_future();
            std::promise<void> go_promise;
            auto go = go_promise.get_future().share();

            auto worker = std::jthread([&](std::stop_token) {
                unlog::prewarm_thread();
                ready_promise.set_value();
                go.wait();
                unlog::info(route, "prewarm-worker-message");
            });

            ready.wait();
            CHECK(test_helper::producer_count() == 2u);

            go_promise.set_value();
            worker.join();

            unlog::flush();
            CHECK(test_helper::producer_count() == 2u);
            REQUIRE_CONTAINS{"prewarm-worker-message"};
        }
    }

    TEST_CASE("007 - global config rejects invalid thread buffer size", "[007][runtime][global-config]") {
        runtime_state_guard guard;

        CHECK_THROWS_AS(set_global_config(global_config{.thread_bufsize = 1u}), std::invalid_argument);
    }

    TEST_CASE("007 - channel rejects max_record_size too small for record metadata", "[007][runtime][channel]") {
        runtime_state_guard guard;

        auto cfg = config<options::max_record_size<1>>::make("tiny-record-limit");
        CHECK_THROWS_AS(make_channel(cfg), std::invalid_argument);
    }

    TEST_CASE("007 - channel rejects max_record_size larger than runtime queue slot", "[007][runtime][channel]") {
        runtime_state_guard guard;

        auto cfg = config<options::max_record_size<8192>>::make("oversize-record-limit");
        CHECK_THROWS_AS(make_channel(cfg), std::invalid_argument);
    }

    TEST_CASE("007 - reset_runtime_for_test clears runtime", "[007][runtime][reset]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("reset-runtime");
        make_channel(cfg);

        CHECK(runtime_ready() == true);
        CHECK(consumer_thread_started() == true);

        reset_runtime_for_test();

        CHECK(runtime_ready() == false);
        CHECK(consumer_thread_started() == false);
        CHECK(get_global_config().thread_bufsize == options::default_thread_bufsize);
    }

    TEST_CASE("007 - setup remains mutable before first emitted log", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto first_cfg = config<>::make("pre-activate-first");
        auto second_cfg = config<>::make("pre-activate-second");

        CHECK_NOTHROW(make_channel(first_cfg));
        CHECK_NOTHROW(make_channel(second_cfg));

        std::stringstream stream;
        CHECK_NOTHROW(detail::add_sink(first_cfg, std::make_shared<backend::ostream_sink_sc>(stream)));
    }

#if UNLOG_DIAGNOSTIC
    TEST_CASE("007 - dropped first log does not activate runtime", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("drop-first");
        auto route = make_channel(cfg);

        auto before = detail::backend_stats();
        auto jumbo = std::string(8192, 'x');
        unlog::info(route, "{}", jumbo);
        auto after_drop = detail::backend_stats();

        CHECK(after_drop.emitted == before.emitted);
        CHECK(after_drop.dropped == (before.dropped + 1u));

        auto pre_activate_cfg = config<>::make("still-open-after-drop");
        CHECK_NOTHROW(make_channel(pre_activate_cfg));

        std::stringstream pre_activate_stream;
        CHECK_NOTHROW(detail::add_sink(cfg, std::make_shared<backend::ostream_sink_sc>(pre_activate_stream)));

        unlog::info(route, "activate-runtime");
        auto after_emit = detail::backend_stats();
        CHECK(after_emit.emitted == (after_drop.emitted + 1u));

        auto late_cfg = config<>::make("late-after-emit");
        CHECK_THROWS_AS(make_channel(late_cfg), std::invalid_argument);

        std::stringstream late_stream;
        CHECK_THROWS_AS(
                detail::add_sink(cfg, std::make_shared<backend::ostream_sink_sc>(late_stream)), std::invalid_argument);
    }
#endif

    TEST_CASE("007 - runtime blocks late setup after first emitted log", "[007][runtime][activation]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("activate-runtime");
        auto route = make_channel(cfg);

        unlog::info(route, "activate-runtime");

        auto next_cfg = config<>::make("late-register");
        CHECK_THROWS_AS(make_channel(next_cfg), std::invalid_argument);

        std::stringstream stream;
        CHECK_THROWS_AS(
                detail::add_sink(cfg, std::make_shared<backend::ostream_sink_sc>(stream)), std::invalid_argument);
    }

    TEST_CASE("007 - global config is locked after channel setup", "[007][runtime][global-config]") {
        runtime_state_guard guard;

        auto cfg = config<>::make("lock-global-config");
        make_channel(cfg);

        CHECK_THROWS_AS(set_global_config(global_config{.thread_bufsize = 1 << 21}), std::invalid_argument);
    }

}  // namespace un::log::test
