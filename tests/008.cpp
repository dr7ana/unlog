#include "utils.hpp"

#include <string>

namespace un::log::test {

    TEST_CASE("008 - fixed record slot respects configured size budget", "[008][record][slot]") {
        using slot_t = backend::basic_record_slot<256>;

        STATIC_REQUIRE(backend::valid_record_slot_v<256>);
        STATIC_REQUIRE(sizeof(slot_t) <= backend::record_slot_storage_size(256));
        STATIC_REQUIRE(slot_t::payload_capacity == backend::payload_capacity_for_record_slot_limit(256).value());

        CHECK(slot_t::storage_size == backend::record_slot_storage_size(256));
    }

    TEST_CASE("008 - runtime record limit helpers reject unsupported budgets", "[008][record][limit]") {
        STATIC_REQUIRE(backend::runtime_record_limit_status(1u) == backend::runtime_record_limit_result::too_small);
        STATIC_REQUIRE(
                backend::runtime_record_limit_status(8192u) ==
                backend::runtime_record_limit_result::exceeds_runtime_slot);
        STATIC_REQUIRE(
                backend::runtime_record_limit_status(options::default_max_record_size) ==
                backend::runtime_record_limit_result::supported);

        CHECK_FALSE(backend::max_message_size_for_runtime_record_limit(1u).has_value());
        CHECK_FALSE(backend::max_message_size_for_runtime_record_limit(8192u).has_value());
        CHECK(backend::max_message_size_for_runtime_record_limit(options::default_max_record_size).has_value());
    }

    TEST_CASE("008 - fixed record slot stores inline metadata and payload", "[008][record][slot]") {
        auto slot = backend::basic_record_slot<256>{};
        auto loc = detail::source_loc{
                .filename = "slot_test.cpp",
                .line = 42,
                .function = "record_slot_test",
        };

        auto result = backend::write_record_slot(
                slot,
                channel_id{7},
                log_level::warn,
                123u,
                456u,
                789u,
                loc,
                OverflowPolicy::drop,
                "{} {}",
                "hello",
                42);

        REQUIRE(result == backend::record_slot_write_result::written);
        CHECK(slot.header.channel == channel_id{7});
        CHECK(slot.level() == log_level::warn);
        CHECK(slot.header.timestamp == 123u);
        CHECK(slot.header.thread_id == 456u);
        CHECK(slot.header.sequence == 789u);
        CHECK(slot.source_location() == loc);
        CHECK(slot.message() == "hello 42"sv);
        CHECK_FALSE(slot.header.truncated);
    }

    TEST_CASE("008 - fixed record slot drops oversize payloads", "[008][record][slot][overflow]") {
        auto slot = backend::basic_record_slot<128>{};
        auto big = std::string(512u, 'x');

        auto result = backend::write_record_slot(
                slot,
                channel_id{1},
                log_level::info,
                1u,
                2u,
                3u,
                detail::source_loc{
                        .filename = "slot_drop.cpp",
                        .line = 9,
                        .function = "drop_case",
                },
                OverflowPolicy::drop,
                "{}",
                big);

        REQUIRE(result == backend::record_slot_write_result::dropped);
        CHECK(slot.message().empty());
        CHECK_FALSE(slot.header.truncated);
    }

    TEST_CASE("008 - fixed record slot truncates oversize payloads with marker", "[008][record][slot][overflow]") {
        auto slot = backend::basic_record_slot<128>{};
        auto big = std::string(512u, 'x');

        auto result = backend::write_record_slot(
                slot,
                channel_id{3},
                log_level::err,
                11u,
                22u,
                33u,
                detail::source_loc{
                        .filename = "slot_truncate.cpp",
                        .line = 17,
                        .function = "truncate_case",
                },
                OverflowPolicy::truncate,
                "prefix-{}",
                big);

        REQUIRE(result == backend::record_slot_write_result::truncated);
        REQUIRE(slot.header.truncated);
        REQUIRE(slot.header.payload_size == slot.capacity());
        REQUIRE(slot.message().ends_with(backend::record_slot_truncate_marker));
        REQUIRE(slot.message().starts_with("prefix-"sv));
    }

}  // namespace un::log::test
