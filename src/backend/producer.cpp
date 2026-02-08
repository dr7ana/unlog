#include "unlog/backend/producer.hpp"

#include <mutex>
#include <thread>
#include <vector>

namespace un::log::backend {
    namespace detail {
        struct producer_registry {
            std::mutex mutex;
            std::vector<std::weak_ptr<producer>> producers;
        };

        producer_registry& registry() {
            static producer_registry value;
            return value;
        }

        uint64_t current_thread_id() {
            return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

        std::vector<producer_ptr> snapshot_locked_reaped(producer_registry& reg) {
            std::vector<producer_ptr> out;
            out.reserve(reg.producers.size());

            auto it = reg.producers.begin();
            while (it != reg.producers.end()) {
                if (auto strong = it->lock()) {
                    out.push_back(std::move(strong));
                    ++it;
                }
                else {
                    it = reg.producers.erase(it);
                }
            }

            return out;
        }

        std::vector<producer_ptr> snapshot_reaped() {
            auto& reg = registry();
            std::lock_guard lock{reg.mutex};
            return snapshot_locked_reaped(reg);
        }

        void register_producer(const producer_ptr& p) {
            auto& reg = registry();
            std::lock_guard lock{reg.mutex};
            reg.producers.emplace_back(p);
        }

        producer_ptr make_tls_producer() {
            auto created = std::make_shared<producer>(current_thread_id());
            register_producer(created);
            return created;
        }

        static thread_local producer_ptr tls_producer;

    }  // namespace detail

    producer::producer(uint64_t thread_id) : thread_id_{thread_id} {}

    uint64_t producer::thread_id() const noexcept {
        return thread_id_;
    }

    ring_buffer& producer::ring() noexcept {
        return ring_;
    }

    const ring_buffer& producer::ring() const noexcept {
        return ring_;
    }

    uint64_t producer::next_sequence() noexcept {
        return sequence_++;
    }

    void producer::count_emitted() noexcept {
        emitted_.fetch_add(1, std::memory_order_relaxed);
    }

    void producer::count_dropped() noexcept {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    void producer::count_truncated() noexcept {
        truncated_.fetch_add(1, std::memory_order_relaxed);
    }

    producer_stats producer::stats() const noexcept {
        return producer_stats{
                .emitted = emitted_.load(std::memory_order_relaxed),
                .dropped = dropped_.load(std::memory_order_relaxed),
                .truncated = truncated_.load(std::memory_order_relaxed),
        };
    }

    producer& get_thread_producer() {
        if (!detail::tls_producer)
            detail::tls_producer = detail::make_tls_producer();

        return *detail::tls_producer;
    }

    std::vector<producer_ptr> producer_snapshot() {
        return detail::snapshot_reaped();
    }

    void for_each_producer(std::function<void(producer&)> visitor) {
        if (!visitor)
            return;

        auto snapshot = producer_snapshot();
        for (auto& p : snapshot)
            visitor(*p);
    }

    size_t active_producer_count() {
        return producer_snapshot().size();
    }

}  // namespace un::log::backend
