#include "unlog/backend/ring.hpp"

#include <algorithm>
#include <cstring>

namespace un::log::backend {

    ring_buffer::ring_buffer() :
            capacity_{thread_ring_capacity}, mask_{capacity_ - 1u}, storage_words_(capacity_ / sizeof(uint64_t)) {}

    size_t ring_buffer::capacity() const noexcept {
        return capacity_;
    }

    uint64_t ring_buffer::produced() const noexcept {
        return tail_.load(std::memory_order_acquire);
    }

    uint64_t ring_buffer::consumed() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    uint64_t ring_buffer::used() const noexcept {
        auto tail = tail_.load(std::memory_order_acquire);
        auto head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    uint64_t ring_buffer::free() const noexcept {
        return capacity_ - used();
    }

    std::optional<ring_buffer::reservation> ring_buffer::try_reserve(
            size_t payload_size,
            const char* logger_name,
            const char* source_file,
            const char* source_function,
            int32_t source_line,
            log_level level,
            uint64_t timestamp,
            uint64_t thread_id,
            uint64_t sequence) noexcept {
        auto total_size = align_record_size(sizeof(record_header) + payload_size);
        if (total_size > capacity_)
            return std::nullopt;

        auto head = head_.load(std::memory_order_acquire);
        auto tail = tail_.load(std::memory_order_relaxed);
        auto used_bytes = tail - head;
        if (used_bytes > capacity_)
            return std::nullopt;

        auto free_bytes = static_cast<uint64_t>(capacity_) - used_bytes;
        if (free_bytes < total_size)
            return std::nullopt;

        auto write_index = static_cast<size_t>(tail & mask_);
        auto contiguous = capacity_ - write_index;

        if (total_size > contiguous) {
            auto required = static_cast<uint64_t>(total_size) + static_cast<uint64_t>(contiguous);
            if (required > free_bytes)
                return std::nullopt;

            if (contiguous >= sizeof(record_header)) {
                auto* padding = header_at(write_index);
                init_padding(*padding, contiguous, timestamp, thread_id, sequence);
                publish_commit(*padding);
            }

            tail += contiguous;
            tail_.store(tail, std::memory_order_release);
            write_index = 0;
        }

        auto* header = header_at(write_index);
        init_data(
                *header,
                total_size,
                payload_size,
                logger_name,
                source_file,
                source_function,
                source_line,
                level,
                timestamp,
                thread_id,
                sequence);
        clear_commit(*header);

        auto* payload_ptr = reinterpret_cast<std::byte*>(header) + sizeof(record_header);
        return reservation{
                .header = header,
                .payload = {payload_ptr, payload_size},
                .payload_size = payload_size,
                .start_offset = tail,
                .end_offset = tail + total_size,
        };
    }

    void ring_buffer::commit(const reservation& res) noexcept {
        if (!res || !res.header)
            return;

        auto total_payload = res.header->total_size - sizeof(record_header);
        if (res.payload_size < total_payload) {
            auto* pad_begin = res.payload.data() + res.payload_size;
            auto pad_bytes = total_payload - res.payload_size;
            std::memset(pad_begin, 0, pad_bytes);
        }

        publish_commit(*res.header);
        tail_.store(res.end_offset, std::memory_order_release);
    }

    std::optional<record_view> ring_buffer::try_peek() const noexcept {
        auto head = head_.load(std::memory_order_relaxed);
        auto tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return std::nullopt;

        auto read_index = static_cast<size_t>(head & mask_);
        auto contiguous = capacity_ - read_index;
        if (contiguous < sizeof(record_header))
            return std::nullopt;

        auto available = tail - head;
        auto window = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(contiguous), available));
        auto bytes = std::span<const std::byte>{byte_data() + read_index, window};
        auto view = try_make_record_view(bytes);
        if (!view.has_value())
            return std::nullopt;
        if (!view->committed())
            return std::nullopt;
        if (view->header->total_size > available)
            return std::nullopt;
        if (view->header->total_size > contiguous)
            return std::nullopt;
        return view;
    }

    bool ring_buffer::consume_peeked(const record_view& view) noexcept {
        if (!view.header)
            return false;

        auto size = view.header->total_size;
        if (size == 0 || size > capacity_)
            return false;

        auto head = head_.load(std::memory_order_relaxed);
        auto read_index = static_cast<size_t>(head & mask_);
        if (view.header != header_at(read_index))
            return false;

        head_.store(head + size, std::memory_order_release);
        return true;
    }

    bool ring_buffer::skip_wrap_gap() noexcept {
        auto head = head_.load(std::memory_order_relaxed);
        auto tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return false;

        auto read_index = static_cast<size_t>(head & mask_);
        auto contiguous = capacity_ - read_index;
        if (contiguous >= sizeof(record_header))
            return false;

        auto available = tail - head;
        auto skip = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(contiguous), available));
        if (skip == 0)
            return false;

        head_.store(head + skip, std::memory_order_release);
        return true;
    }

    std::span<std::byte> ring_buffer::bytes() noexcept {
        return {byte_data(), capacity_};
    }

    std::span<const std::byte> ring_buffer::bytes() const noexcept {
        return {byte_data(), capacity_};
    }

    record_header* ring_buffer::header_at(size_t index) noexcept {
        return reinterpret_cast<record_header*>(byte_data() + index);
    }

    const record_header* ring_buffer::header_at(size_t index) const noexcept {
        return reinterpret_cast<const record_header*>(byte_data() + index);
    }

    std::byte* ring_buffer::byte_data() noexcept {
        return reinterpret_cast<std::byte*>(storage_words_.data());
    }

    const std::byte* ring_buffer::byte_data() const noexcept {
        return reinterpret_cast<const std::byte*>(storage_words_.data());
    }

    void ring_buffer::init_padding(
            record_header& header,
            size_t total_size,
            uint64_t timestamp,
            uint64_t thread_id,
            uint64_t sequence) noexcept {
        header.version = record_abi_version;
        header.kind = record_kind::padding;
        header.header_size = static_cast<uint8_t>(sizeof(record_header));
        header.total_size = total_size;
        header.payload_size = 0;
        header.logger_name = nullptr;
        header.source_file = nullptr;
        header.source_function = nullptr;
        header.source_line = 0;
        header.thread_id = thread_id;
        header.sequence = sequence;
        header.level = encode_level(log_level::off);
        header.timestamp = timestamp;
        clear_commit(header);
    }

    void ring_buffer::init_data(
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
            uint64_t sequence) noexcept {
        header.version = record_abi_version;
        header.kind = record_kind::data;
        header.header_size = static_cast<uint8_t>(sizeof(record_header));
        header.total_size = total_size;
        header.payload_size = payload_size;
        header.logger_name = logger_name;
        header.source_file = source_file;
        header.source_function = source_function;
        header.source_line = source_line;
        header.thread_id = thread_id;
        header.sequence = sequence;
        header.level = encode_level(level);
        header.timestamp = timestamp;
    }

}  // namespace un::log::backend
