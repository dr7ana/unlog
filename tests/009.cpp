#include "utils.hpp"

#include <memory>

namespace un::log::test {

    inline constexpr auto queue_test_config = global_config{
            .thread_bufsize = 4096u,
            .huge_thread_bufsize = options::default_huge_thread_bufsize,
            .max_record_size = 256u,
            .max_producers = 64u,
    };

    TEST_CASE("009 - configured queue derives capacity at compile time", "[009][queue][traits]") {
        using traits = backend::configured_queue_traits<queue_test_config, false>;

        STATIC_REQUIRE(sizeof(typename traits::record_slot) == queue_test_config.max_record_size);
        STATIC_REQUIRE(traits::buffer_size == queue_test_config.thread_bufsize);
        STATIC_REQUIRE(traits::capacity == 16u);
    }

    TEST_CASE("009 - queue producer stores fixed record slots through spsc queue", "[009][queue][producer]") {
        using producer_t = backend::configured_queue_producer<queue_test_config, false>;
        using record_slot_t = producer_t::record_slot;

        auto producer = producer_t{};
        auto route = detail::route_state{channel_id{9}, "queue-slot"};
        auto source_location = detail::source_loc{
                .filename = "queue_slot.cpp",
                .line = 77,
                .function = "queue_slot_case",
        };

        bool wrote_slot = false;
        auto produced = producer.queue().produce([&](record_slot_t* slot) noexcept -> bool {
            std::construct_at(
                    slot,
                    &route,
                    log_level::debug,
                    123u,
                    source_location.filename,
                    static_cast<int32_t>(source_location.line));
            auto result = backend::write_record_slot(*slot, OverflowPolicy::drop, "queue-{}", 7);
            wrote_slot = result == backend::record_slot_write_result::written;
            if (!wrote_slot) {
                std::destroy_at(slot);
                return false;
            }

            return true;
        });

        REQUIRE(produced);
        REQUIRE(wrote_slot);

        size_t consumed = 0;
        auto drain_count = producer.queue().consume_all([&](record_slot_t& slot) {
            ++consumed;
            CHECK(slot.header.route == &route);
            CHECK(slot.level() == log_level::debug);
            CHECK(slot.header.timestamp == 123u);
            CHECK(slot.header.source_file == source_location.filename);
            CHECK(slot.header.source_line == source_location.line);
            CHECK(slot.message() == "queue-7"sv);
        });

        CHECK(drain_count == 1u);
        CHECK(consumed == 1u);
        CHECK(producer.queue().empty());
    }

    TEST_CASE("009 - queue producer callback can cancel publish", "[009][queue][producer]") {
        using producer_t = backend::configured_queue_producer<queue_test_config, false>;
        using record_slot_t = producer_t::record_slot;

        auto producer = producer_t{};
        bool invoked = false;
        auto produced = producer.queue().produce([&](record_slot_t* slot) noexcept -> bool {
            invoked = true;
            std::construct_at(slot, nullptr, log_level::debug, 0u, nullptr, 0);
            std::destroy_at(slot);
            return false;
        });

        CHECK_FALSE(produced);
        CHECK(invoked);
        CHECK(producer.queue().empty());
    }

}  // namespace un::log::test
