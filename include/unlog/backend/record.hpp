#pragma once

#include "unlog/config.hpp"
#include "unlog/format.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace un::log::backend {

    inline constexpr uint16_t record_abi_version = 1;
    inline constexpr size_t record_alignment = 8;
    static_assert(record_alignment <= alignof(uint64_t));

    enum class record_kind : uint8_t {
        data = 1,
        padding = 2,
    };

    struct alignas(record_alignment) record_header {
        uint16_t version{record_abi_version};
        record_kind kind{record_kind::data};
        uint8_t header_size{static_cast<uint8_t>(sizeof(record_header))};

        size_t total_size{0};
        size_t committed_size{0};  // published last with release semantics
        size_t payload_size{0};

        const char* logger_name{nullptr};
        const char* source_file{nullptr};
        const char* source_function{nullptr};
        int32_t source_line{0};

        uint64_t thread_id{0};
        uint64_t sequence{0};

        uint8_t level{static_cast<uint8_t>(log_level::info)};
        uint64_t timestamp{0};
    };

    static_assert(std::is_standard_layout_v<record_header>);
    static_assert(std::is_trivially_copyable_v<record_header>);
    static_assert(alignof(record_header) == record_alignment);

    inline constexpr size_t align_record_size(size_t size) noexcept {
        auto align = static_cast<size_t>(record_alignment);
        return (size + (align - 1u)) & ~(align - 1u);
    }

    inline constexpr size_t payload_size_for(std::string_view message) noexcept {
        return message.size();
    }

    inline constexpr size_t total_record_size_for_payload(size_t payload_size) noexcept {
        return align_record_size(sizeof(record_header) + payload_size);
    }

    inline constexpr std::optional<size_t> max_message_size_for_record_limit(size_t max_record_size) noexcept {
        auto header_size = sizeof(record_header);
        auto min_total = total_record_size_for_payload(0);
        if (max_record_size < min_total)
            return std::nullopt;

        auto max_payload = max_record_size - header_size;
        while (max_payload > 0 && total_record_size_for_payload(max_payload) > max_record_size)
            --max_payload;

        if (total_record_size_for_payload(max_payload) > max_record_size)
            return std::nullopt;

        return max_payload;
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
        channel_id channel{invalid_channel_id};
        uint64_t timestamp{0};
        uint64_t thread_id{0};
        uint64_t sequence{0};

        const char* source_file{nullptr};
        const char* source_function{nullptr};
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

        record_slot_header header{};
        std::array<char, payload_storage_size> payload{};

        constexpr void reset(
                channel_id channel,
                log_level level_value,
                uint64_t timestamp_value,
                uint64_t thread_id_value,
                uint64_t sequence_value,
                const ::un::log::detail::source_loc& source_location) noexcept {
            header.channel = channel;
            header.timestamp = timestamp_value;
            header.thread_id = thread_id_value;
            header.sequence = sequence_value;
            header.source_file = source_location.filename;
            header.source_function = source_location.function;
            header.source_line = static_cast<int32_t>(source_location.line);
            header.payload_size = 0;
            header.level = encode_level(level_value);
            header.truncated = false;
        }

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
                    .function = header.source_function,
            };
        }
    };

    template <size_t MaxRecordSize>
    inline constexpr bool valid_record_slot_v =
            payload_capacity_for_record_slot_limit(MaxRecordSize).has_value() &&
            sizeof(basic_record_slot<MaxRecordSize>) <= record_slot_storage_size(MaxRecordSize);

    using runtime_record_slot = basic_record_slot<options::default_max_record_size>;

    enum class runtime_record_limit_result : uint8_t {
        supported = 0,
        too_small = 1,
        exceeds_runtime_slot = 2,
    };

    inline constexpr runtime_record_limit_result runtime_record_limit_status(size_t max_record_size) noexcept {
        auto max_message_size = max_message_size_for_record_limit(max_record_size);
        if (!max_message_size.has_value())
            return runtime_record_limit_result::too_small;

        if (*max_message_size > runtime_record_slot::payload_capacity)
            return runtime_record_limit_result::exceeds_runtime_slot;

        return runtime_record_limit_result::supported;
    }

    inline constexpr std::optional<size_t> max_message_size_for_runtime_record_limit(size_t max_record_size) noexcept {
        auto max_message_size = max_message_size_for_record_limit(max_record_size);
        if (!max_message_size.has_value())
            return std::nullopt;

        if (*max_message_size > runtime_record_slot::payload_capacity)
            return std::nullopt;

        return max_message_size;
    }

    template <size_t MaxRecordSize, typename... Arg>
    inline record_slot_write_result write_record_slot(
            basic_record_slot<MaxRecordSize>& slot,
            channel_id channel,
            log_level level,
            uint64_t timestamp,
            uint64_t thread_id,
            uint64_t sequence,
            const ::un::log::detail::source_loc& source_location,
            OverflowPolicy overflow_policy,
            size_t payload_limit,
            fmt::format_string<Arg...> format,
            Arg&&... args) {

        slot.reset(channel, level, timestamp, thread_id, sequence, source_location);
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
            channel_id channel,
            log_level level,
            uint64_t timestamp,
            uint64_t thread_id,
            uint64_t sequence,
            const ::un::log::detail::source_loc& source_location,
            OverflowPolicy overflow_policy,
            fmt::format_string<Arg...> format,
            Arg&&... args) {
        return write_record_slot(
                slot,
                channel,
                level,
                timestamp,
                thread_id,
                sequence,
                source_location,
                overflow_policy,
                slot.capacity(),
                format,
                std::forward<Arg>(args)...);
    }

    inline void clear_commit(record_header& header) noexcept {
        std::atomic_ref<size_t>{header.committed_size}.store(0, std::memory_order_relaxed);
    }

    inline void publish_commit(record_header& header) noexcept {
        std::atomic_ref<size_t>{header.committed_size}.store(header.total_size, std::memory_order_release);
    }

    inline size_t committed_size_acquire(const record_header& header) noexcept {
        auto& commit_word = const_cast<size_t&>(header.committed_size);
        return std::atomic_ref<size_t>{commit_word}.load(std::memory_order_acquire);
    }

    struct record_view {
        const record_header* header{nullptr};
        std::span<const std::byte> payload{};

        constexpr bool has_header() const noexcept { return header != nullptr; }

        bool committed() const noexcept {
            return has_header() && committed_size_acquire(*header) == header->total_size;
        }

        constexpr bool is_padding() const noexcept { return has_header() && header->kind == record_kind::padding; }

        constexpr log_level level() const noexcept {
            return has_header() ? decode_level(header->level) : log_level::off;
        }
    };

    inline std::optional<record_view> try_make_record_view(std::span<const std::byte> bytes) {
        if (bytes.size() < sizeof(record_header))
            return std::nullopt;

        auto ptr = bytes.data();
        if ((reinterpret_cast<std::uintptr_t>(ptr) % alignof(record_header)) != 0u)
            return std::nullopt;

        auto header = std::bit_cast<const record_header*>(ptr);
        if (header->version != record_abi_version)
            return std::nullopt;
        if (header->header_size != sizeof(record_header))
            return std::nullopt;
        if (header->total_size < sizeof(record_header))
            return std::nullopt;
        if (header->total_size > bytes.size())
            return std::nullopt;
        auto max_payload_size = header->total_size - sizeof(record_header);
        if (header->payload_size > max_payload_size)
            return std::nullopt;

        auto payload_ptr = ptr + sizeof(record_header);
        auto payload_size = max_payload_size;

        return record_view{
                .header = header,
                .payload = {payload_ptr, payload_size},
        };
    }

}  // namespace un::log::backend
