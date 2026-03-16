#include "utils.hpp"

#include <memory>

namespace un::log::test {

    TEST_CASE("009 - queue runtime traits derive power-of-two capacity from thread buffer", "[009][queue][traits]") {
        using traits = backend::queue_runtime_traits<256>;
        auto capacity = traits::queue_capacity_for(4096);

        REQUIRE(capacity.has_value());
        CHECK(capacity.value() == 16u);
        STATIC_REQUIRE(traits::slot_size <= 256u);
        CHECK(backend::queue_slot_capacity_for(4096, traits::slot_size) == capacity);
        STATIC_REQUIRE(backend::queue_slot_capacity_for(traits::slot_size - 1u, traits::slot_size) == std::nullopt);
    }

    TEST_CASE("009 - queue producer stores fixed record slots through spsc queue", "[009][queue][producer]") {
        using producer_t = backend::queue_producer<256>;
        using record_slot_t = producer_t::record_slot;

        auto producer = producer_t{42u, 4096u};
        auto source_location = detail::source_loc{
                .filename = "queue_slot.cpp",
                .line = 77,
                .function = "queue_slot_case",
        };

        bool wrote_slot = false;
        auto produced = producer.queue().produce([&](record_slot_t* slot) noexcept -> bool {
            std::construct_at(slot);
            auto result = backend::write_record_slot(
                    *slot,
                    channel_id{9},
                    log_level::debug,
                    123u,
                    producer.thread_id(),
                    producer.next_sequence(),
                    source_location,
                    OverflowPolicy::drop,
                    "queue-{}",
                    7);
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
            CHECK(slot.header.channel == channel_id{9});
            CHECK(slot.level() == log_level::debug);
            CHECK(slot.header.timestamp == 123u);
            CHECK(slot.header.thread_id == producer.thread_id());
            CHECK(slot.header.sequence == 0u);
            CHECK(slot.source_location() == source_location);
            CHECK(slot.message() == "queue-7"sv);
        });

        CHECK(drain_count == 1u);
        CHECK(consumed == 1u);
        CHECK(producer.queue().empty());
    }

    TEST_CASE("009 - queue producer callback can cancel publish", "[009][queue][producer]") {
        using producer_t = backend::queue_producer<256>;
        using record_slot_t = producer_t::record_slot;

        auto producer = producer_t{42u, 4096u};
        bool invoked = false;
        auto produced = producer.queue().produce([&](record_slot_t* slot) noexcept -> bool {
            invoked = true;
            std::construct_at(slot);
            std::destroy_at(slot);
            return false;
        });

        CHECK_FALSE(produced);
        CHECK(invoked);
        CHECK(producer.queue().empty());
    }

}  // namespace un::log::test
