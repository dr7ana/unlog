#include "utils.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace un::log::test {

    TEST_CASE("010 - runtime reset races concurrent loggers", "[010][runtime][shutdown][stress]") {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        constexpr auto cycles = int{8};
        constexpr auto worker_count = size_t{8};

        for (auto cycle = 0; cycle < cycles; ++cycle) {
            auto cfg = config<options::threadsafe>::make_fd(null_sink.get(), "teardown-stress");
            auto route = test_log::make_channel(cfg);
            test_log::start();

            auto stop = std::atomic<bool>{false};
            auto logged = std::atomic<size_t>{0};

            {
                auto workers = std::vector<std::jthread>{};
                workers.reserve(worker_count);
                for (auto t = size_t{0}; t < worker_count; ++t) {
                    workers.emplace_back([&route, &stop, &logged, cycle](std::stop_token) {
                        auto count = size_t{0};
                        while (!stop.load(std::memory_order_relaxed)) {
                            unlog::info(route, "cycle {} message {}", cycle, count++);
                            logged.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                }

                // every worker must be inside the produce loop before the runtime is torn
                // down underneath it
                while (logged.load(std::memory_order_relaxed) < worker_count) {
                    std::this_thread::yield();
                }

                reset_runtime_for_test();

                // keep logging against the retired state for a beat before stopping
                auto after_reset = logged.load(std::memory_order_relaxed);
                while (logged.load(std::memory_order_relaxed) < after_reset + worker_count) {
                    std::this_thread::yield();
                }

                stop.store(true, std::memory_order_relaxed);
            }
        }

        CHECK(consumer_thread_started() == false);
    }

    TEST_CASE("010 - stale channel handle is inert after reset", "[010][runtime][shutdown]") {
        auto null_sink = socket_wrapper::null_sink();
        runtime_state_guard guard;

        auto cfg = config<>::make_fd(null_sink.get(), "stale-handle");
        auto route = test_log::make_channel(cfg);
        test_log::start();
        unlog::info(route, "pre-reset");

        reset_runtime_for_test();

        CHECK_NOTHROW(unlog::info(route, "post-reset"));
        CHECK(consumer_thread_started() == false);
    }

}  // namespace un::log::test
