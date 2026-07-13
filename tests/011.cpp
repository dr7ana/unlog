#include "utils.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace un::log::test {

    class blocking_handoff_sink final : public backend::sink {
      public:
        void write(std::string_view) override {
            if (writes_.fetch_add(1, std::memory_order_relaxed) == 0) {
                first_write_started_.release();
                release_first_write_.acquire();
                return;
            }
            second_write_started_.release();
        }

        void flush() override {}

        [[nodiscard]] bool wait_for_first_write() {
            return first_write_started_.try_acquire_for(std::chrono::seconds{1});
        }

        void release_first_write() { release_first_write_.release(); }

        [[nodiscard]] bool wait_for_second_write() {
            return second_write_started_.try_acquire_for(std::chrono::seconds{1});
        }

      private:
        std::atomic<size_t> writes_{0};
        std::binary_semaphore first_write_started_{0};
        std::binary_semaphore release_first_write_{0};
        std::binary_semaphore second_write_started_{0};
    };

    template <typename... Opt>
    static void require_bounded_drain_handoff() {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        auto observer = std::make_shared<blocking_handoff_sink>();
        auto cfg = config<Opt...>::make_fd(null_sink.get(), "bounded-drain");
        auto route = test_log::make_channel(cfg);
        detail::add_sink<test_log::config>(config<>::make("bounded-drain-observer"), observer);
        test_log::start();

        unlog::info(route, "first bounded-drain record");
        auto first_write_started = observer->wait_for_first_write();
        if (first_write_started) {
            unlog::info(route, "second bounded-drain record");
        }
        observer->release_first_write();

        REQUIRE(first_write_started);
        REQUIRE(observer->wait_for_second_write());
    }

    TEST_CASE("011 - channels emit only to their configured sinks", "[011][consumer][routing]") {
        auto first_pair = socket_wrapper::make_pair();
        auto second_pair = socket_wrapper::make_pair();
        runtime_state_guard guard;

        auto first = test_log::make_channel(config<>::make_fd(first_pair[0].get(), "first-route"));
        auto second = test_log::make_channel(config<>::make_fd(second_pair[0].get(), "second-route"));
        test_log::start();

        unlog::info(first, "first-only-marker");
        unlog::info(second, "second-only-marker");
        test_log::flush();

        auto first_output = first_pair[1].read_available();
        auto second_output = second_pair[1].read_available();
        REQUIRE_FALSE(first_output.empty());
        REQUIRE_FALSE(second_output.empty());
        CHECK(first_output.contains("first-only-marker"));
        CHECK_FALSE(first_output.contains("second-only-marker"));
        CHECK(second_output.contains("second-only-marker"));
        CHECK_FALSE(second_output.contains("first-only-marker"));
    }

#ifndef NDEBUG
    TEST_CASE("011 - flush completes under concurrent producer load", "[011][runtime][flush][stress]") {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        auto cfg = config<options::threadsafe>::make_fd(null_sink.get(), "flush-load");
        auto route = test_log::make_channel(cfg);
        test_log::start();

        constexpr auto worker_count = size_t{4};
        constexpr auto flush_count = size_t{100};
        auto stop = std::atomic<bool>{false};
        auto logged = std::atomic<size_t>{0};

        {
            auto workers = std::vector<std::jthread>{};
            workers.reserve(worker_count);
            for (auto t = size_t{0}; t < worker_count; ++t) {
                workers.emplace_back([&route, &stop, &logged](std::stop_token) {
                    auto i = size_t{0};
                    while (!stop.load(std::memory_order_relaxed)) {
                        unlog::info(route, "flush-load message {}", i++);
                        logged.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }

            while (logged.load(std::memory_order_relaxed) < worker_count) {
                std::this_thread::yield();
            }
            for (auto i = size_t{0}; i < flush_count; ++i) {
                test_log::flush();
            }
            stop.store(true, std::memory_order_relaxed);
            workers.clear();
            test_log::flush();
        }

        SUCCEED("all flushes completed under load");
    }
#endif

    TEST_CASE("011 - bounded drain hands off records beyond its tail snapshot", "[011][runtime][wakeup]") {
        SECTION("single-threaded") {
            require_bounded_drain_handoff<>();
        }
        SECTION("threadsafe") {
            require_bounded_drain_handoff<options::threadsafe>();
        }
    }

    TEST_CASE("011 - burst tail drains with no further producer activity", "[011][runtime][wakeup]") {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        // 1 KiB slots: 1 MiB per producer queue = 1024 slots, so a 500-record burst per
        // worker can never overflow its queue and drop-policy losses cannot mask a stranded
        // tail.

        util::capture_test_logs(log_level::info);
        auto cfg = config<options::threadsafe>::make_fd(null_sink.get(), "burst-tail");
        auto route = test_log::make_channel(cfg);
        test_log::start();

        constexpr auto worker_count = size_t{4};
        constexpr auto burst = size_t{500};

        {
            auto workers = std::vector<std::jthread>{};
            workers.reserve(worker_count);
            for (auto t = size_t{0}; t < worker_count; ++t) {
                workers.emplace_back([&route, t](std::stop_token) {
                    for (auto i = size_t{0}; i < burst; ++i) {
                        unlog::info(route, "burst-marker {} {}", t, i);
                    }
                });
            }
        }

        // Producers are done; nothing will nudge the consumer again. Every record must
        // already be drained or surfaced by this single flush.
        auto output = util::captured_output();

        auto occurrences = size_t{0};
        auto pos = output.find("burst-marker");
        while (pos != std::string::npos) {
            ++occurrences;
            pos = output.find("burst-marker", pos + 1);
        }

        CHECK(occurrences == worker_count * burst);
    }

    TEST_CASE("011 - %g renders the source basename at the consumer", "[011][consumer][pattern]") {
        runtime_state_guard guard;

        util::capture_test_logs(log_level::info);
        auto cfg = config<>::make("basename-check");
        auto route = test_log::make_channel(cfg);
        test_log::start();

        unlog::info(route, "basename-marker");
        auto output = util::captured_output();

        REQUIRE(output.contains("basename-marker"));
        // Producers forward the full source path; the strip is the consumer's job now.
        CHECK(output.contains("011.cpp"));
        CHECK_FALSE(output.contains("/011.cpp"));
    }

}  // namespace un::log::test
