#pragma once

#include "record.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace un::log::test {
    struct test_helper;
}

namespace un::log::backend {

    inline constexpr size_t thread_ring_capacity = size_t{1} << 20;  // 1 MiB per thread
    static_assert((thread_ring_capacity & (thread_ring_capacity - 1u)) == 0u);
    static_assert(thread_ring_capacity >= (2u * sizeof(record_header)));

    class ring_buffer {
        friend struct un::log::test::test_helper;

      public:
        struct reservation {
            record_header* header{nullptr};
            std::span<std::byte> payload{};
            size_t payload_size{0};
            uint64_t start_offset{0};
            uint64_t end_offset{0};

            constexpr explicit operator bool() const noexcept { return header != nullptr; }
        };

        ring_buffer();

        [[nodiscard]] size_t capacity() const noexcept;

        [[nodiscard]] uint64_t produced() const noexcept;
        [[nodiscard]] uint64_t consumed() const noexcept;

        [[nodiscard]] uint64_t used() const noexcept;

        [[nodiscard]] uint64_t free() const noexcept;

        [[nodiscard]] std::optional<reservation> try_reserve(
                size_t payload_size,
                const char* logger_name,
                const char* source_file,
                const char* source_function,
                int32_t source_line,
                log_level level,
                uint64_t timestamp,
                uint64_t thread_id,
                uint64_t sequence) noexcept;

        void commit(const reservation& res) noexcept;

        [[nodiscard]] std::optional<record_view> try_peek() const noexcept;

        bool consume_peeked(const record_view& view) noexcept;

        bool skip_wrap_gap() noexcept;

        std::span<std::byte> bytes() noexcept;
        std::span<const std::byte> bytes() const noexcept;

      private:
        record_header* header_at(size_t index) noexcept;

        const record_header* header_at(size_t index) const noexcept;

        std::byte* byte_data() noexcept;

        const std::byte* byte_data() const noexcept;

        static void init_padding(
                record_header& header,
                size_t total_size,
                uint64_t timestamp,
                uint64_t thread_id,
                uint64_t sequence) noexcept;

        static void init_data(
                record_header& header,
                size_t total_size,
                size_t payload_size,
                const char* logger_name,
                const char* source_file,
                const char* source_function,
                int32_t source_line,
                log_level level,
                uint64_t timestamp,
                uint64_t thread_id,
                uint64_t sequence) noexcept;

        size_t capacity_;
        size_t mask_;
        std::vector<uint64_t> storage_words_;

        alignas(64) std::atomic<uint64_t> head_{0};
        alignas(64) std::atomic<uint64_t> tail_{0};
    };

}  // namespace un::log::backend
