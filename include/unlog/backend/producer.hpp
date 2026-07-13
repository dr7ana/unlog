#pragma once

#include "backend.hpp"
#include "record.hpp"

#include "utl/spsc/queue.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace un::log::backend {

    class diagnostic_thread_owner {
      public:
        void diagnostic_assert_thread();

      private:
#if UNLOG_DIAGNOSTIC
        std::atomic<uint64_t> owner_{0};
#endif
    };

    struct alignas(64) producer_active_word {
        std::atomic<uint64_t> bits{0};
    };

    using runtime_record_consumer = void (*)(void*, const record_slot_header&, std::string_view);
    using runtime_waker = void (*)(void*) noexcept;

    class runtime_producer_backend {
      public:
        virtual ~runtime_producer_backend() = default;
        virtual void bind_waker(void* context, runtime_waker waker) noexcept = 0;
        virtual void register_channel(RuntimeMode mode, bool huge_pages) noexcept = 0;
        virtual void prepare_start() = 0;
        virtual void prewarm_thread() = 0;
        // Returns true if all ready producers were drained to empty;
        // false if another drain call is required.
        virtual bool drain_ready(void* context, runtime_record_consumer consume) = 0;
        virtual void drain_flush(void* context, runtime_record_consumer consume) = 0;
        virtual void drain_until_idle(void* context, runtime_record_consumer consume) = 0;
        virtual size_t producer_count() const noexcept = 0;
#if UNLOG_DIAGNOSTIC
        virtual producer_stats stats() const noexcept = 0;
#endif
    };

    template <global_config Config, bool HugePages>
    struct configured_queue_traits {
        using record_slot = basic_record_slot<Config.max_record_size>;
        static_assert(valid_record_slot_v<Config.max_record_size>);
        static_assert(sizeof(record_slot) == Config.max_record_size);

        static constexpr size_t buffer_size = HugePages ? Config.huge_thread_bufsize : Config.thread_bufsize;
        static_assert(buffer_size % sizeof(record_slot) == 0);
        static constexpr size_t capacity = buffer_size / sizeof(record_slot);
        static_assert(std::has_single_bit(capacity));

        using queue_type = utl::spsc_queue<record_slot, capacity, utl::spsc_allocator<record_slot, HugePages>>;
    };

    template <global_config Config, bool HugePages>
    class configured_queue_producer : private diagnostic_thread_owner {
        enum class lifecycle : uint8_t { active, retired, free };

      public:
        using diagnostic_thread_owner::diagnostic_assert_thread;
        using traits = configured_queue_traits<Config, HugePages>;
        using record_slot = typename traits::record_slot;
        using queue_type = typename traits::queue_type;

        configured_queue_producer() = default;
        [[nodiscard]] queue_type& queue() noexcept { return queue_; }
        [[nodiscard]] const queue_type& queue() const noexcept { return queue_; }
        [[nodiscard]] size_t producer_index() const noexcept { return producer_index_; }
        [[nodiscard]] size_t next_free_index() const noexcept { return next_free_index_; }
        void set_next_free_index(size_t value) noexcept { next_free_index_ = value; }

        void bind_active_word(producer_active_word& active_word, size_t producer_index) noexcept {
            active_word_ = &active_word;
            producer_index_ = producer_index;
            active_bit_mask_ = uint64_t{1} << (producer_index % std::numeric_limits<uint64_t>::digits);
        }

        [[nodiscard]] bool try_mark_enqueued() noexcept { return !enqueued_.exchange(true, std::memory_order_acq_rel); }
        void clear_enqueued() noexcept { enqueued_.exchange(false, std::memory_order_acq_rel); }
        void publish_ready_bit() noexcept {
            if (active_word_) {
                active_word_->bits.fetch_or(active_bit_mask_, std::memory_order_release);
            }
        }

        [[nodiscard]] bool retire() noexcept {
            auto expected = lifecycle::active;
            return lifecycle_.compare_exchange_strong(
                    expected, lifecycle::retired, std::memory_order_release, std::memory_order_relaxed);
        }

        [[nodiscard]] bool try_mark_free() noexcept {
            auto expected = lifecycle::retired;
            return lifecycle_.compare_exchange_strong(
                    expected, lifecycle::free, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        [[nodiscard]] bool is_retired() const noexcept {
            return lifecycle_.load(std::memory_order_relaxed) == lifecycle::retired;
        }

        void reactivate() noexcept {
            next_free_index_ = Config.max_producers;
            enqueued_.store(false, std::memory_order_relaxed);
            lifecycle_.store(lifecycle::active, std::memory_order_release);
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

      private:
        producer_active_word* active_word_{nullptr};
        size_t producer_index_{0};
        size_t next_free_index_{Config.max_producers};
        uint64_t active_bit_mask_{0};
        queue_type queue_{};
        std::atomic<bool> enqueued_{false};
        std::atomic<lifecycle> lifecycle_{lifecycle::active};
#if UNLOG_DIAGNOSTIC
        std::atomic<uint64_t> emitted_{0};
        std::atomic<uint64_t> dropped_{0};
        std::atomic<uint64_t> truncated_{0};
#endif
    };

    template <global_config Config>
    class configured_producer_backend final : public runtime_producer_backend {
        static constexpr size_t producers_per_word{std::numeric_limits<uint64_t>::digits};
        static_assert(std::has_single_bit(producers_per_word));
        static constexpr auto producer_word_shift{std::bit_width(producers_per_word) - 1u};
        static constexpr size_t producer_word_mask{producers_per_word - 1u};
        static constexpr size_t active_word_count{(Config.max_producers + producer_word_mask) >> producer_word_shift};
        static constexpr size_t invalid_producer_index{Config.max_producers};

        template <bool HugePages>
        using producer_type = configured_queue_producer<Config, HugePages>;

        template <bool HugePages>
        struct registry {
            std::mutex mutex;
            std::deque<producer_type<HugePages>> storage;
            std::array<producer_type<HugePages>*, Config.max_producers> slots{};
            std::array<producer_active_word, active_word_count> active_words{};
            size_t free_head{invalid_producer_index};
            std::atomic<size_t> count{0};
        };

      public:
        using normal_producer = producer_type<false>;
        using huge_producer = producer_type<true>;

        void bind_waker(void* context, runtime_waker waker) noexcept override {
            wake_context_ = context;
            waker_ = waker;
        }

        void register_channel(RuntimeMode mode, bool huge_pages) noexcept override {
            if (mode == RuntimeMode::threadsafe) {
                (huge_pages ? threadsafe_huge_channels_ : threadsafe_normal_channels_)++;
            }
            else if (huge_pages) {
                single_huge_needed_ = true;
            }
            else {
                single_normal_needed_ = true;
            }
        }

        void prepare_start() override {
            if (single_normal_needed_ && !single_normal_) {
                single_normal_ = std::make_unique<normal_producer>();
            }
            if (single_huge_needed_ && !single_huge_) {
                single_huge_ = std::make_unique<huge_producer>();
            }
        }

        void prewarm_thread() override {
            if (threadsafe_normal_channels_ != 0) {
                threadsafe_producer<false>();
            }
            if (threadsafe_huge_channels_ != 0) {
                threadsafe_producer<true>();
            }
        }

        template <typename Policy>
        auto& producer() {
            if constexpr (Policy::runtime_mode == RuntimeMode::threadsafe) {
                return threadsafe_producer<Policy::huge_pages>();
            }
            else if constexpr (Policy::huge_pages) {
                if constexpr (UNLOG_DIAGNOSTIC) {
                    single_huge_->diagnostic_assert_thread();
                }
                return *single_huge_;
            }
            else {
                if constexpr (UNLOG_DIAGNOSTIC) {
                    single_normal_->diagnostic_assert_thread();
                }
                return *single_normal_;
            }
        }

        template <typename Policy>
        void note_work(auto& producer) noexcept {
            if (!producer.try_mark_enqueued()) {
                return;
            }
            if constexpr (Policy::runtime_mode == RuntimeMode::threadsafe) {
                producer.publish_ready_bit();
            }
            if (waker_) {
                waker_(wake_context_);
            }
        }

        template <bool HugePages>
        void retire_threadsafe_producer(producer_type<HugePages>& producer) noexcept {
            if (!producer.retire()) {
                return;
            }
            producer.publish_ready_bit();
            if (waker_) {
                waker_(wake_context_);
            }
        }

        bool drain_ready(void* context, runtime_record_consumer consume) override {
            auto all_drained = true;
            if (single_normal_) {
                all_drained &= drain_producer(*single_normal_, context, consume);
            }
            if (single_huge_) {
                all_drained &= drain_producer(*single_huge_, context, consume);
            }
            all_drained &= drain_registry(normal_registry_, context, consume);
            all_drained &= drain_registry(huge_registry_, context, consume);
            return all_drained;
        }

        void drain_flush(void* context, runtime_record_consumer consume) override {
            if (single_normal_) {
                drain_producer(*single_normal_, context, consume);
            }
            if (single_huge_) {
                drain_producer(*single_huge_, context, consume);
            }
            drain_registry_flush(normal_registry_, context, consume);
            drain_registry_flush(huge_registry_, context, consume);
        }

        void drain_until_idle(void* context, runtime_record_consumer consume) override {
            while (!drain_ready(context, consume)) {
            }
        }

        size_t producer_count() const noexcept override {
            return normal_registry_.count.load(std::memory_order_acquire) +
                   huge_registry_.count.load(std::memory_order_acquire);
        }

#if UNLOG_DIAGNOSTIC
        producer_stats stats() const noexcept override {
            auto result = producer_stats{};
            auto append = [&result](const auto& producer) {
                auto value = producer.stats();
                result.emitted += value.emitted;
                result.dropped += value.dropped;
                result.truncated += value.truncated;
            };
            if (single_normal_) {
                append(*single_normal_);
            }
            if (single_huge_) {
                append(*single_huge_);
            }
            append_registry_stats(normal_registry_, append);
            append_registry_stats(huge_registry_, append);
            return result;
        }
#endif

      private:
        template <bool HugePages>
        registry<HugePages>& registry_for() noexcept {
            if constexpr (HugePages) {
                return huge_registry_;
            }
            else {
                return normal_registry_;
            }
        }

        template <bool HugePages>
        producer_type<HugePages>& threadsafe_producer() {
            struct tls_state {
                configured_producer_backend* owner{nullptr};
                producer_type<HugePages>* producer{nullptr};

                void release() noexcept {
                    if (owner && producer) {
                        owner->template retire_threadsafe_producer<HugePages>(*producer);
                    }
                    owner = nullptr;
                    producer = nullptr;
                }

                ~tls_state() { release(); }
            };
            static thread_local tls_state tls;
            if (tls.owner == this && tls.producer) {
                return *tls.producer;
            }

            tls.release();

            auto& registry = registry_for<HugePages>();
            std::lock_guard lock{registry.mutex};
            auto* producer = static_cast<producer_type<HugePages>*>(nullptr);
            if (registry.free_head != invalid_producer_index) {
                auto producer_index = registry.free_head;
                producer = registry.slots[producer_index];
                registry.free_head = producer->next_free_index();
                producer->reactivate();
            }
            else {
                auto producer_index = registry.storage.size();
                if (producer_index >= Config.max_producers) {
                    throw std::runtime_error{"threadsafe producer capacity exceeded"};
                }
                registry.storage.emplace_back();
                producer = &registry.storage.back();
                producer->bind_active_word(
                        registry.active_words[producer_index >> producer_word_shift], producer_index);
                registry.slots[producer_index] = producer;
                registry.count.store(producer_index + 1u, std::memory_order_release);
            }
            tls.owner = this;
            tls.producer = producer;
            return *producer;
        }

        static bool finish_drain(auto& producer) {
            if (!producer.queue().empty()) {
                producer.publish_ready_bit();
                return false;
            }
            producer.clear_enqueued();
            if (producer.queue().empty()) {
                return true;
            }
            if (producer.try_mark_enqueued()) {
                producer.publish_ready_bit();
            }
            return false;
        }

        static bool drain_producer(auto& producer, void* context, runtime_record_consumer consume) {
            producer.queue().consume_all([&](auto& slot) { consume(context, slot.header, slot.message()); });
            return finish_drain(producer);
        }

        template <bool HugePages>
        static bool drain_registry_producer(
                registry<HugePages>& registry,
                producer_type<HugePages>& producer,
                void* context,
                runtime_record_consumer consume) {
            auto all_drained = drain_producer(producer, context, consume);
            if (all_drained && producer.is_retired() && producer.try_mark_free()) {
                std::lock_guard lock{registry.mutex};
                producer.set_next_free_index(registry.free_head);
                registry.free_head = producer.producer_index();
            }
            return all_drained;
        }

        template <bool HugePages>
        static bool drain_registry(registry<HugePages>& registry, void* context, runtime_record_consumer consume) {
            auto all_drained = true;
            for (size_t word_index = 0,
                        words = (registry.count.load(std::memory_order_acquire) + producer_word_mask) >>
                                producer_word_shift;
                 word_index < words;
                 ++word_index) {
                for (auto pending = registry.active_words[word_index].bits.exchange(0, std::memory_order_acq_rel);
                     pending != 0;
                     pending &= pending - 1u) {
                    all_drained &= drain_registry_producer(
                            registry,
                            *registry.slots[(word_index << producer_word_shift) + std::countr_zero(pending)],
                            context,
                            consume);
                }
            }
            return all_drained;
        }

        template <bool HugePages>
        static void drain_registry_flush(
                registry<HugePages>& registry, void* context, runtime_record_consumer consume) {
            for (size_t i = 0, count = registry.count.load(std::memory_order_acquire); i < count; ++i) {
                drain_registry_producer(registry, *registry.slots[i], context, consume);
            }
        }

#if UNLOG_DIAGNOSTIC
        static void append_registry_stats(const auto& registry, const auto& append) noexcept {
            for (size_t i = 0, count = registry.count.load(std::memory_order_acquire); i < count; ++i) {
                append(*registry.slots[i]);
            }
        }
#endif

        void* wake_context_{nullptr};
        runtime_waker waker_{nullptr};
        size_t threadsafe_normal_channels_{0};
        size_t threadsafe_huge_channels_{0};
        bool single_normal_needed_{false};
        bool single_huge_needed_{false};
        std::unique_ptr<normal_producer> single_normal_;
        std::unique_ptr<huge_producer> single_huge_;
        registry<false> normal_registry_;
        registry<true> huge_registry_;
    };

}  // namespace un::log::backend
