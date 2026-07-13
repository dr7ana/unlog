#include "unlog/backend/producer.hpp"

namespace un::log::backend {

    void diagnostic_thread_owner::diagnostic_assert_thread() {
#if UNLOG_DIAGNOSTIC
        static std::atomic<uint64_t> next{1};
        thread_local uint64_t thread_id = next.fetch_add(1, std::memory_order_relaxed);

        auto owner = owner_.load(std::memory_order_relaxed);
        if (owner == thread_id) {
            return;
        }
        if (owner == 0 &&
            owner_.compare_exchange_strong(owner, thread_id, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
        throw std::logic_error{"single-threaded producer used from multiple threads"};
#endif
    }

}  // namespace un::log::backend
