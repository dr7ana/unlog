#pragma once

#include "unlog.hpp"

#include "unlog/backend/producer.hpp"
#include "unlog/backend/ring.hpp"
#include "unlog/backend/sinks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <ostream>
#include <utility>
#include <vector>

namespace un::log::test {

    using namespace un::log::literals;

    void get_runtime_backend(const std::function<void()>& fn);
    void get_runtime_sqpoll_backend(const std::function<void(backend::sqpoll_backend&)>& fn);
    void reset_runtime_for_test();

    struct test_helper {
        template <typename... Opt>
            requires detail::valid_opt_pack<options::sqpoll_live, Opt...>
        static auto make_sqpoll_config(
                std::string_view name = "unlog"sv,
                SinkType sink_type = SinkType::cout,
                uint8_t flags = Flags::color,
                std::optional<std::string> format = std::nullopt,
                std::optional<int> output_fd = std::nullopt,
                std::optional<fs::path> unix_dgram_path = std::nullopt) {
            return basic_sqpoll_config<Opt...>{
                    name,
                    sink_type,
                    flags,
                    std::move(format),
                    std::move(output_fd),
                    std::move(unix_dgram_path),
            };
        }

        static void reset_runtime_state() {
            reset_runtime_for_test();
            drain_all_records();
        }

        static void reset_ring(backend::ring_buffer& ring) {
            ring.head_.store(0, std::memory_order_relaxed);
            ring.tail_.store(0, std::memory_order_relaxed);
        }

        static void reset_producer(backend::producer& producer) {
            reset_ring(producer.ring());
            producer.sequence_ = 0;
            producer.emitted_.store(0, std::memory_order_relaxed);
            producer.dropped_.store(0, std::memory_order_relaxed);
            producer.truncated_.store(0, std::memory_order_relaxed);
        }

        static void drain_all_records() {
            for (;;) {
                auto result = backend::drain_batch(
                        2048, [](backend::producer&, const backend::record_view&) { return true; });

                if (result.drained_records == 0 && result.skipped_padding == 0)
                    break;
            }

            backend::for_each_producer([](backend::producer& producer) { reset_producer(producer); });
        }

        static void add_live_endpoint(backend::sqpoll_backend& sqpoll_backend, backend::sqpoll_endpoint endpoint) {
            std::lock_guard lock{sqpoll_backend.mutex_};
            sqpoll_backend.runtime_.endpoints.push_back(std::move(endpoint));
        }

        static void set_live_completion_failures(backend::sqpoll_backend& sqpoll_backend, uint64_t failures) {
            std::lock_guard lock{sqpoll_backend.mutex_};
            sqpoll_backend.runtime_.completion_failures = failures;
        }

        static ClockType live_clock_type(backend::sqpoll_backend& sqpoll_backend) {
            std::lock_guard lock{sqpoll_backend.mutex_};
            return sqpoll_backend.clock_type_;
        }

        static void set_live_clock_type(backend::sqpoll_backend& sqpoll_backend, ClockType clock_type) {
            std::lock_guard lock{sqpoll_backend.mutex_};
            sqpoll_backend.clock_type_ = clock_type;
        }
    };

    struct runtime_state_guard {
        runtime_state_guard() { test_helper::reset_runtime_state(); }

        ~runtime_state_guard() { test_helper::reset_runtime_state(); }
    };

    struct util {
        static std::stringstream stream;

        static auto reset() {
            unlog::flush();
            test_helper::drain_all_records();
            stream = {};
            stream.clear();
        }

        static auto capture_test_logs(log_level level = get_default_level()) {
            reset();
            set_default_level(level);
            auto conf = config<>::make_sqpoll("capture");
            detail::add_sink(conf, std::make_shared<backend::ostream_sink>(stream));
        }

        template <detail::basic_config_type Conf>
        static auto capture_test_logs(const Conf& conf, log_level level = get_default_level()) {
            reset();
            set_default_level(level);
            detail::add_sink(conf, std::make_shared<backend::ostream_sink>(stream));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_CONTAINS {
        REQUIRE_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            REQUIRE(util::stream.str().contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_CONTAINS {
        CHECK_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            CHECK(util::stream.str().contains(msg));
        }
    };

    struct REQUIRE_EMPTY {
        REQUIRE_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            REQUIRE(util::stream.str().empty());
        }
    };

    struct CHECK_EMPTY {
        CHECK_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << util::stream.str());
            CHECK(util::stream.str().empty());
        }
    };

    // deduction guides
    template <typename T>
    REQUIRE_CONTAINS(T msg) -> REQUIRE_CONTAINS<T>;
    template <typename T>
    CHECK_CONTAINS(T msg) -> CHECK_CONTAINS<T>;

}  // namespace un::log::test
