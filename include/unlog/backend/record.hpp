#pragma once

#include "unlog/config.hpp"
#include "unlog/format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

namespace un::log::detail {
    struct route_state;
}

namespace un::log::backend {

    inline constexpr size_t record_alignment = 8;
    static_assert(record_alignment <= alignof(uint64_t));

    inline constexpr size_t align_record_size(size_t size) noexcept {
        auto align = static_cast<size_t>(record_alignment);
        return (size + (align - 1u)) & ~(align - 1u);
    }

    inline constexpr uint8_t encode_level(log_level level) noexcept {
        return static_cast<uint8_t>(level);
    }

    inline constexpr log_level decode_level(uint8_t level) noexcept {
        if (level > static_cast<uint8_t>(log_level::off))
            return log_level::off;
        return static_cast<log_level>(level);
    }

    struct record_slot_header {
        const detail::route_state* route{nullptr};
        uint64_t timestamp{0};
        const char* source_file{nullptr};
        int32_t source_line{0};

        uint32_t payload_size{0};
        uint8_t level{encode_level(log_level::info)};
        bool truncated{false};
    };

    static_assert(std::is_standard_layout_v<record_slot_header>);
    static_assert(std::is_trivially_copyable_v<record_slot_header>);

    inline constexpr auto record_slot_truncate_marker = "[truncated]"sv;

    inline constexpr size_t record_slot_storage_size(size_t max_record_size) noexcept {
        return align_record_size(max_record_size);
    }

    inline constexpr std::optional<size_t> payload_capacity_for_record_slot_limit(size_t max_record_size) noexcept {
        auto storage_size = record_slot_storage_size(max_record_size);
        auto header_size = align_record_size(sizeof(record_slot_header));
        if (storage_size < header_size)
            return std::nullopt;

        return storage_size - header_size;
    }

    enum class record_slot_write_result : uint8_t {
        written = 0,
        dropped = 1,
        truncated = 2,
    };

    namespace slot_detail {
        inline void write_truncate_marker(char* out, size_t size) noexcept {
            if (!out || size == 0)
                return;

            if (size <= record_slot_truncate_marker.size()) {
                std::memcpy(out, record_slot_truncate_marker.data(), size);
                return;
            }

            auto marker_offset = size - record_slot_truncate_marker.size();
            std::memcpy(out + marker_offset, record_slot_truncate_marker.data(), record_slot_truncate_marker.size());
        }
    }  // namespace slot_detail

    template <size_t MaxRecordSize>
    struct alignas(record_alignment) basic_record_slot {
        static constexpr size_t storage_size = record_slot_storage_size(MaxRecordSize);
        static constexpr size_t payload_capacity =
                payload_capacity_for_record_slot_limit(MaxRecordSize).value_or(size_t{0});
        static constexpr size_t payload_storage_size = payload_capacity == 0 ? size_t{1} : payload_capacity;

        record_slot_header header;
        std::array<char, payload_storage_size> payload;

        // Construction initializes the header only; the payload stays indeterminate until
        // write_record_slot formats into it and sets header.payload_size, which bounds every
        // read. Value-initializing the payload here would zero-fill the whole slot on every
        // construct_at in the produce path.
        constexpr basic_record_slot(
                const detail::route_state* route,
                log_level level_value,
                uint64_t timestamp_value,
                const char* source_file_value,
                int32_t source_line_value) noexcept :
                header{
                        .route = route,
                        .timestamp = timestamp_value,
                        .source_file = source_file_value,
                        .source_line = source_line_value,
                        .payload_size = 0,
                        .level = encode_level(level_value),
                        .truncated = false,
                } {}

        [[nodiscard]] constexpr char* payload_data() noexcept { return payload.data(); }
        [[nodiscard]] constexpr const char* payload_data() const noexcept { return payload.data(); }
        [[nodiscard]] constexpr size_t capacity() const noexcept { return payload_capacity; }
        [[nodiscard]] constexpr std::string_view message() const noexcept {
            return {payload.data(), static_cast<size_t>(header.payload_size)};
        }
        [[nodiscard]] constexpr log_level level() const noexcept { return decode_level(header.level); }
        [[nodiscard]] constexpr ::un::log::detail::source_loc source_location() const noexcept {
            return ::un::log::detail::source_loc{
                    .filename = header.source_file,
                    .line = header.source_line,
                    .function = nullptr,
            };
        }
    };

    template <size_t MaxRecordSize>
    inline constexpr bool valid_record_slot_v =
            payload_capacity_for_record_slot_limit(MaxRecordSize).has_value() &&
            sizeof(basic_record_slot<MaxRecordSize>) <= record_slot_storage_size(MaxRecordSize);

    template <size_t MaxRecordSize, typename... Arg>
    inline record_slot_write_result write_record_slot(
            basic_record_slot<MaxRecordSize>& slot,
            OverflowPolicy overflow_policy,
            size_t payload_limit,
            fmt::format_string<Arg...> format,
            Arg&&... args) {

        auto effective_limit = std::min(payload_limit, slot.capacity());

        auto result = fmt::format_to_n(slot.payload_data(), effective_limit, format, std::forward<Arg>(args)...);
        auto required_size = static_cast<size_t>(result.size);

        if (required_size <= effective_limit) {
            slot.header.payload_size = static_cast<uint32_t>(required_size);
            return record_slot_write_result::written;
        }

        if (overflow_policy == OverflowPolicy::drop) {
            slot.header.payload_size = 0;
            return record_slot_write_result::dropped;
        }

        slot.header.payload_size = static_cast<uint32_t>(effective_limit);
        slot.header.truncated = true;
        slot_detail::write_truncate_marker(slot.payload_data(), effective_limit);
        return record_slot_write_result::truncated;
    }

    template <size_t MaxRecordSize, typename... Arg>
    inline record_slot_write_result write_record_slot(
            basic_record_slot<MaxRecordSize>& slot,
            OverflowPolicy overflow_policy,
            fmt::format_string<Arg...> format,
            Arg&&... args) {
        return write_record_slot(slot, overflow_policy, slot.capacity(), format, std::forward<Arg>(args)...);
    }

}  // namespace un::log::backend
