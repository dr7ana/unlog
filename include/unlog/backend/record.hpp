#pragma once

#include "unlog/config.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
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
