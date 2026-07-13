#include "utils.hpp"

#include <string>

namespace un::log::test {

    TEST_CASE("008 - fixed record slot respects configured size budget", "[008][record][slot]") {
        using slot_t = backend::basic_record_slot<256>;

        STATIC_REQUIRE(sizeof(backend::record_slot_header) == 40u);
        STATIC_REQUIRE(backend::valid_record_slot_v<256>);
        STATIC_REQUIRE(sizeof(slot_t) <= backend::record_slot_storage_size(256));
        STATIC_REQUIRE(slot_t::payload_capacity == backend::payload_capacity_for_record_slot_limit(256).value());

        CHECK(slot_t::storage_size == backend::record_slot_storage_size(256));
    }

    TEST_CASE("008 - compile-time record budget derives inline payload capacity", "[008][record][limit]") {
        using default_slot = backend::basic_record_slot<options::default_max_record_size>;

        STATIC_REQUIRE(backend::valid_record_slot_v<options::default_max_record_size>);
        STATIC_REQUIRE(default_slot::storage_size == options::default_max_record_size);
        STATIC_REQUIRE(default_slot::payload_capacity > 0u);
        STATIC_REQUIRE(
                default_slot::payload_capacity ==
                options::default_max_record_size - backend::align_record_size(sizeof(backend::record_slot_header)));
        STATIC_REQUIRE(backend::payload_capacity_for_record_slot_limit(sizeof(backend::record_slot_header) - 1u) == 0u);
    }

    TEST_CASE("008 - fixed record slot stores inline metadata and payload", "[008][record][slot]") {
        auto loc = detail::source_loc{
                .filename = "slot_test.cpp",
                .line = 42,
                .function = "record_slot_test",
        };
        auto route = detail::route_state{channel_id{7}, "slot-test"};
        auto slot = backend::basic_record_slot<256>{
                &route, log_level::warn, 123u, loc.filename, static_cast<int32_t>(loc.line)};

        auto result = backend::write_record_slot(slot, OverflowPolicy::drop, "{} {}", "hello", 42);

        REQUIRE(result == backend::record_slot_write_result::written);
        CHECK(slot.header.route == &route);
        CHECK(slot.level() == log_level::warn);
        CHECK(slot.header.timestamp == 123u);
        CHECK(slot.header.source_file == loc.filename);
        CHECK(slot.header.source_line == loc.line);
        CHECK(slot.source_location().function == nullptr);
        CHECK(slot.message() == "hello 42"sv);
        CHECK_FALSE(slot.header.truncated);
    }

    TEST_CASE("008 - fixed record slot drops oversize payloads", "[008][record][slot][overflow]") {
        auto loc = detail::source_loc{
                .filename = "slot_drop.cpp",
                .line = 9,
                .function = "drop_case",
        };
        auto slot = backend::basic_record_slot<128>{
                nullptr, log_level::info, 1u, loc.filename, static_cast<int32_t>(loc.line)};
        auto big = std::string(512u, 'x');

        auto result = backend::write_record_slot(slot, OverflowPolicy::drop, "{}", big);

        REQUIRE(result == backend::record_slot_write_result::dropped);
        CHECK(slot.message().empty());
        CHECK_FALSE(slot.header.truncated);
    }

    TEST_CASE("008 - fixed record slot truncates oversize payloads with marker", "[008][record][slot][overflow]") {
        auto loc = detail::source_loc{
                .filename = "slot_truncate.cpp",
                .line = 17,
                .function = "truncate_case",
        };
        auto slot = backend::basic_record_slot<128>{
                nullptr, log_level::err, 11u, loc.filename, static_cast<int32_t>(loc.line)};
        auto big = std::string(512u, 'x');

        auto result = backend::write_record_slot(slot, OverflowPolicy::truncate, "prefix-{}", big);

        REQUIRE(result == backend::record_slot_write_result::truncated);
        REQUIRE(slot.header.truncated);
        REQUIRE(slot.header.payload_size == slot.capacity());
        REQUIRE(slot.message().ends_with(backend::record_slot_truncate_marker));
        REQUIRE(slot.message().starts_with("prefix-"sv));
    }

}  // namespace un::log::test
