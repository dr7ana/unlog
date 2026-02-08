#pragma once

#include "ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace un::log::backend {

    struct producer_stats {
        uint64_t emitted{0};
        uint64_t dropped{0};
        uint64_t truncated{0};

        constexpr bool operator==(const producer_stats& obj) const {
            return emitted == obj.emitted && dropped == obj.dropped && truncated == obj.truncated;
        }
    };

    class producer {
        friend struct un::log::test::test_helper;

      public:
        explicit producer(uint64_t thread_id);

        [[nodiscard]] uint64_t thread_id() const noexcept;
        [[nodiscard]] ring_buffer& ring() noexcept;
        [[nodiscard]] const ring_buffer& ring() const noexcept;

        [[nodiscard]] uint64_t next_sequence() noexcept;
        void count_emitted() noexcept;
        void count_dropped() noexcept;
        void count_truncated() noexcept;
        [[nodiscard]] producer_stats stats() const noexcept;

      private:
        uint64_t thread_id_{0};
        uint64_t sequence_{0};
        ring_buffer ring_{};
        std::atomic<uint64_t> emitted_{0};
        std::atomic<uint64_t> dropped_{0};
        std::atomic<uint64_t> truncated_{0};
    };

    using producer_ptr = std::shared_ptr<producer>;

    struct drain_result {
        size_t producer_count{0};
        size_t drained_records{0};
        size_t skipped_padding{0};
        bool hit_limit{false};
    };

    producer& get_thread_producer();

    std::vector<producer_ptr> producer_snapshot();

    void for_each_producer(std::function<void(producer&)> visitor);

    template <typename Callback>
    inline drain_result drain_local_batch(producer& owner, size_t max_records, Callback&& cb) {
        drain_result result{};
        if (max_records == 0)
            return result;

        result.producer_count = 1;
        auto& ring = owner.ring();
        while (result.drained_records < max_records) {
            auto maybe_view = ring.try_peek();
            if (!maybe_view.has_value()) {
                if (ring.skip_wrap_gap())
                    continue;
                break;
            }

            const auto& view = *maybe_view;
            if (view.is_padding()) {
                if (!ring.consume_peeked(view))
                    break;
                ++result.skipped_padding;
                continue;
            }

            auto keep_going = static_cast<bool>(cb(owner, view));

            if (!ring.consume_peeked(view))
                break;

            ++result.drained_records;
            if (!keep_going) {
                result.hit_limit = true;
                return result;
            }
        }

        result.hit_limit = result.drained_records >= max_records;
        return result;
    }

    template <typename Callback>
    inline bool drain_producer_ring(producer& owner, size_t max_records, Callback&& cb, drain_result& result) {
        auto& ring = owner.ring();
        while (result.drained_records < max_records) {
            auto maybe_view = ring.try_peek();
            if (!maybe_view.has_value()) {
                if (ring.skip_wrap_gap())
                    continue;
                break;
            }

            const auto& view = *maybe_view;
            if (view.is_padding()) {
                if (!ring.consume_peeked(view))
                    break;
                ++result.skipped_padding;
                continue;
            }

            auto keep_going = static_cast<bool>(cb(owner, view));

            if (!ring.consume_peeked(view))
                break;

            ++result.drained_records;
            if (!keep_going) {
                return true;
            }
        }

        return result.drained_records >= max_records;
    }

    template <typename Callback>
    inline drain_result drain_batch(size_t max_records, Callback&& callback) {
        drain_result result{};
        if (max_records == 0)
            return result;

        auto snapshot = producer_snapshot();
        result.producer_count = snapshot.size();

        for (auto& p : snapshot) {
            if (drain_producer_ring(*p, max_records, callback, result)) {
                result.hit_limit = true;
                return result;
            }
        }

        return result;
    }

    size_t active_producer_count();

}  // namespace un::log::backend
