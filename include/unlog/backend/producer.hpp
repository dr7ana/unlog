#pragma once

#include "backend.hpp"
#include "record.hpp"

#include "utl/spsc/queue.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>

namespace un::log::backend {

    struct alignas(64) producer_active_word {
        std::atomic<uint64_t> bits{0};
    };

    inline constexpr std::optional<size_t> queue_slot_capacity_for(
            size_t thread_buffer_size, size_t record_slot_size) noexcept {
        if (record_slot_size == 0 || thread_buffer_size < record_slot_size) {
            return std::nullopt;
        }

        auto slots = thread_buffer_size / record_slot_size;
        auto capacity = std::bit_floor(slots);
        if (capacity == 0) {
            return std::nullopt;
        }

        return capacity;
    }

    template <size_t MaxRecordSize>
    struct queue_runtime_traits {
        using record_slot = basic_record_slot<MaxRecordSize>;
        static_assert(valid_record_slot_v<MaxRecordSize>);

        static constexpr size_t slot_size = sizeof(record_slot);
        using queue_type = utl::spsc_queue<record_slot, utl::dynamic_extent>;

        [[nodiscard]] static constexpr std::optional<size_t> queue_capacity_for(size_t thread_buffer_size) noexcept {
            return queue_slot_capacity_for(thread_buffer_size, slot_size);
        }
    };

    template <size_t MaxRecordSize>
    class queue_producer {
      public:
        using traits = queue_runtime_traits<MaxRecordSize>;
        using record_slot = typename traits::record_slot;
        using queue_type = typename traits::queue_type;

        explicit queue_producer(uint64_t thread_id, size_t thread_buffer_size) : thread_id_{thread_id} {
            reconfigure(thread_buffer_size);
        }

        [[nodiscard]] uint64_t thread_id() const noexcept { return thread_id_; }
        [[nodiscard]] queue_type& queue() noexcept { return *queue_; }
        [[nodiscard]] const queue_type& queue() const noexcept { return *queue_; }

        [[nodiscard]] uint64_t next_sequence() noexcept { return sequence_++; }
        [[nodiscard]] size_t producer_index() const noexcept { return producer_index_; }
        [[nodiscard]] size_t active_word_index() const noexcept { return active_word_index_; }
        [[nodiscard]] uint64_t active_bit_mask() const noexcept { return active_bit_mask_; }

        void bind_active_word(producer_active_word& active_word, size_t producer_index) noexcept {
            active_word_ = std::ref(active_word);
            producer_index_ = producer_index;
            active_word_index_ = producer_index / 64u;
            active_bit_mask_ = uint64_t{1} << (producer_index % 64u);
        }

        [[nodiscard]] bool try_mark_enqueued() noexcept {
            auto expected = false;
            return enqueued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        void clear_enqueued() noexcept { enqueued_.store(false, std::memory_order_release); }

        [[nodiscard]] bool enqueued() const noexcept { return enqueued_.load(std::memory_order_acquire); }

        void publish_ready_bit() noexcept {
            if (active_word_) {
                active_word_->get().bits.fetch_or(active_bit_mask_, std::memory_order_release);
            }
        }

#if UNLOG_DIAGNOSTIC
        void count_emitted() noexcept { emitted_.fetch_add(1, std::memory_order_relaxed); }
        void count_dropped() noexcept { dropped_.fetch_add(1, std::memory_order_relaxed); }
        void count_truncated() noexcept { truncated_.fetch_add(1, std::memory_order_relaxed); }

        [[nodiscard]] producer_stats stats() const noexcept {
            return producer_stats{
                    .emitted = emitted_.load(std::memory_order_relaxed),
                    .dropped = dropped_.load(std::memory_order_relaxed),
                    .truncated = truncated_.load(std::memory_order_relaxed),
            };
        }
#endif

        void reconfigure(size_t thread_buffer_size) {
            auto queue_capacity = traits::queue_capacity_for(thread_buffer_size);
            if (!queue_capacity.has_value()) {
                throw std::invalid_argument{"global thread_bufsize does not fit one producer record slot"};
            }

            queue_.emplace(*queue_capacity);
            sequence_ = 0;
#if UNLOG_DIAGNOSTIC
            emitted_.store(0, std::memory_order_relaxed);
            dropped_.store(0, std::memory_order_relaxed);
            truncated_.store(0, std::memory_order_relaxed);
#endif
            enqueued_.store(false, std::memory_order_relaxed);
        }

        void reset_for_test() noexcept {
            if (queue_.has_value()) {
                queue_->clear();
            }
            sequence_ = 0;

#if UNLOG_DIAGNOSTIC
            emitted_.store(0, std::memory_order_relaxed);
            dropped_.store(0, std::memory_order_relaxed);
            truncated_.store(0, std::memory_order_relaxed);
#endif
            enqueued_.store(false, std::memory_order_relaxed);
        }

      private:
        uint64_t thread_id_{0};
        uint64_t sequence_{0};
        std::optional<std::reference_wrapper<producer_active_word>> active_word_{};
        size_t producer_index_{0};
        size_t active_word_index_{0};
        uint64_t active_bit_mask_{0};
        std::optional<queue_type> queue_{};
        std::atomic<bool> enqueued_{false};
#if UNLOG_DIAGNOSTIC
        std::atomic<uint64_t> emitted_{0};
        std::atomic<uint64_t> dropped_{0};
        std::atomic<uint64_t> truncated_{0};
#endif
    };

    using runtime_queue_traits = queue_runtime_traits<options::default_max_record_size>;
    using runtime_queue_producer = queue_producer<options::default_max_record_size>;

}  // namespace un::log::backend
