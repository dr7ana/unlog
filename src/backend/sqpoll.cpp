#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace un::log::backend {
    using namespace un::log::literals;

    namespace detail {

        static int check_rv(long rv, std::string_view action) {
            if (rv >= 0)
                return rv;

            auto ec = std::error_code{errno, std::system_category()};
            throw std::system_error{
                    ec,
                    "Error code {} ({}) returned during {} (returned:{})"_format(ec.value(), ec.message(), action, rv)};
        }

#if defined(__linux__)
        uint32_t sq_flags_value(sqpoll_runtime& runtime) noexcept {
            if (!runtime.sq_flags)
                return 0;
            return std::atomic_ref<uint32_t>(*runtime.sq_flags).load(std::memory_order_acquire);
        }

        bool sqpoll_needs_wakeup(sqpoll_runtime& runtime) noexcept {
            return (sq_flags_value(runtime) & IORING_SQ_NEED_WAKEUP) != 0;
        }

        void wake_sqpoll(sqpoll_runtime& runtime) noexcept {
            if (runtime.ring_fd < 0)
                return;
            if (!sqpoll_needs_wakeup(runtime))
                return;

            (void)::syscall(__NR_io_uring_enter, runtime.ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, nullptr, 0);
        }

        int make_ring_fd(uint32_t queue_depth, io_uring_params& params) {
            params.flags = IORING_SETUP_SQPOLL;
            return check_rv(::syscall(__NR_io_uring_setup, queue_depth, &params), "io_uring_setup(SQPOLL)");
        }

        void* map_ptr(int fd, size_t len, off_t offset, std::string_view action) {
            auto ptr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, offset);
            if (ptr != MAP_FAILED)
                return ptr;

            auto ec = std::error_code{errno, std::system_category()};
            throw std::system_error{
                    ec, "Error code {} ({}) returned during {}"_format(ec.value(), ec.message(), action)};
        }

        void unmap_ptr(void* ptr, size_t len) noexcept {
            if (!ptr || ptr == MAP_FAILED || len == 0)
                return;
            ::munmap(ptr, len);
        }

        int open_file_fd(const std::string& path) {
            return check_rv(
                    ::open(path.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC | O_NONBLOCK, 0644), "open(file)");
        }

        void set_fd_nonblocking(int fd) {
            auto flags = check_rv(::fcntl(fd, F_GETFL), "fcntl(F_GETFL)");
            if ((flags & O_NONBLOCK) != 0)
                return;

            check_rv(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), "fcntl(F_SETFL, O_NONBLOCK)");
        }

        sockaddr_un make_unix_dgram_addr(std::string_view path) {
            auto addr = sockaddr_un{};
            addr.sun_family = AF_UNIX;
            if (path.size() >= sizeof(addr.sun_path)) {
                throw std::invalid_argument{"unix_dgram path is too long"};
            }

            std::ranges::copy(path, addr.sun_path);
            addr.sun_path[path.size()] = '\0';
            return addr;
        }

        int open_unix_dgram_fd() {
            return check_rv(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), "socket(AF_UNIX)");
        }

        void set_ring_views(sqpoll_runtime& runtime, const io_uring_params& params) {
            auto sq_base = static_cast<char*>(runtime.sq_ring_ptr);
            runtime.sq_head = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.head);
            runtime.sq_tail = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.tail);
            runtime.sq_ring_mask = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.ring_mask);
            runtime.sq_ring_entries = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.ring_entries);
            runtime.sq_flags = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.flags);
            runtime.sq_dropped = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.dropped);
            runtime.sq_array = reinterpret_cast<uint32_t*>(sq_base + params.sq_off.array);

            auto cq_base = static_cast<char*>(runtime.cq_ring_ptr);
            runtime.cq_head = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.head);
            runtime.cq_tail = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.tail);
            runtime.cq_ring_mask = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.ring_mask);
            runtime.cq_ring_entries = reinterpret_cast<uint32_t*>(cq_base + params.cq_off.ring_entries);
            runtime.cqes = cq_base + params.cq_off.cqes;
        }

        void map_ring_state(sqpoll_runtime& runtime, const io_uring_params& params) {
            runtime.sq_ring_size = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
            runtime.cq_ring_size = params.cq_off.cqes + (params.cq_entries * sizeof(io_uring_cqe));

            if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0) {
                auto combined_size = std::max(runtime.sq_ring_size, runtime.cq_ring_size);
                runtime.sq_ring_ptr = map_ptr(runtime.ring_fd, combined_size, IORING_OFF_SQ_RING, "mmap(SQ/CQ ring)");
                runtime.sq_ring_size = combined_size;
                runtime.cq_ring_ptr = runtime.sq_ring_ptr;
                runtime.cq_ring_size = combined_size;
            }
            else {
                runtime.sq_ring_ptr =
                        map_ptr(runtime.ring_fd, runtime.sq_ring_size, IORING_OFF_SQ_RING, "mmap(SQ ring)");
                runtime.cq_ring_ptr =
                        map_ptr(runtime.ring_fd, runtime.cq_ring_size, IORING_OFF_CQ_RING, "mmap(CQ ring)");
            }

            runtime.sqes_size = params.sq_entries * sizeof(io_uring_sqe);
            runtime.sqes_ptr = map_ptr(runtime.ring_fd, runtime.sqes_size, IORING_OFF_SQES, "mmap(SQEs)");
            set_ring_views(runtime, params);
        }

        void reset_ring_views(sqpoll_runtime& runtime) noexcept {
            runtime.sq_ring_ptr = nullptr;
            runtime.sq_ring_size = 0;
            runtime.cq_ring_ptr = nullptr;
            runtime.cq_ring_size = 0;
            runtime.sqes_ptr = nullptr;
            runtime.sqes_size = 0;
            runtime.sq_head = nullptr;
            runtime.sq_tail = nullptr;
            runtime.sq_ring_mask = nullptr;
            runtime.sq_ring_entries = nullptr;
            runtime.sq_flags = nullptr;
            runtime.sq_dropped = nullptr;
            runtime.sq_array = nullptr;
            runtime.cq_head = nullptr;
            runtime.cq_tail = nullptr;
            runtime.cq_ring_mask = nullptr;
            runtime.cq_ring_entries = nullptr;
            runtime.cqes = nullptr;
        }

        void unmap_ring_state(sqpoll_runtime& runtime) noexcept {
            unmap_ptr(runtime.sqes_ptr, runtime.sqes_size);

            if (runtime.sq_ring_ptr && runtime.sq_ring_ptr == runtime.cq_ring_ptr) {
                unmap_ptr(runtime.sq_ring_ptr, runtime.sq_ring_size);
            }
            else {
                unmap_ptr(runtime.sq_ring_ptr, runtime.sq_ring_size);
                unmap_ptr(runtime.cq_ring_ptr, runtime.cq_ring_size);
            }

            reset_ring_views(runtime);
        }

        bool same_endpoint(const sqpoll_endpoint& lhs, const sqpoll_endpoint& rhs) {
            if (lhs.sink_type != rhs.sink_type)
                return false;

            if (lhs.sink_type == SinkType::fd)
                return lhs.fd == rhs.fd;

            return lhs.path == rhs.path;
        }

        void remove_inflight_by_user_data(sqpoll_runtime& runtime, uint64_t user_data, int res) noexcept {
            for (size_t idx = 0; idx < runtime.inflight.size(); ++idx) {
                auto& entry = runtime.inflight[idx];
                if (entry.user_data != user_data)
                    continue;

                auto expected = entry.expected_size;
                runtime.inflight[idx] = std::move(runtime.inflight.back());
                runtime.inflight.pop_back();

                if (res < 0 || static_cast<size_t>(res) < expected)
                    ++runtime.completion_failures;

                return;
            }

            ++runtime.completion_failures;
        }

        void reap_cqes(sqpoll_runtime& runtime, size_t max_count) noexcept {
            if (!runtime.started || !runtime.cq_head || !runtime.cq_tail || !runtime.cq_ring_mask || !runtime.cqes)
                return;

            auto head = std::atomic_ref<uint32_t>(*runtime.cq_head).load(std::memory_order_acquire);
            auto tail = std::atomic_ref<uint32_t>(*runtime.cq_tail).load(std::memory_order_acquire);
            auto ring_mask = *runtime.cq_ring_mask;
            auto cqes = reinterpret_cast<io_uring_cqe*>(runtime.cqes);
            size_t consumed = 0;

            while (head != tail && consumed < max_count) {
                auto& cqe = cqes[head & ring_mask];
                remove_inflight_by_user_data(runtime, cqe.user_data, cqe.res);
                ++head;
                ++consumed;
            }

            std::atomic_ref<uint32_t>(*runtime.cq_head).store(head, std::memory_order_release);
        }

        bool try_queue_write(sqpoll_runtime& runtime, const sqpoll_endpoint& endpoint, std::string_view line) noexcept {
            if (line.size() > std::numeric_limits<uint32_t>::max())
                return false;

            if (!runtime.sq_head || !runtime.sq_tail || !runtime.sq_ring_entries || !runtime.sq_ring_mask ||
                !runtime.sq_array || !runtime.sqes_ptr)
                return false;

            auto sq_head = std::atomic_ref<uint32_t>(*runtime.sq_head).load(std::memory_order_acquire);
            auto sq_tail = std::atomic_ref<uint32_t>(*runtime.sq_tail).load(std::memory_order_relaxed);
            auto ring_entries = *runtime.sq_ring_entries;
            if ((sq_tail - sq_head) >= ring_entries)
                return false;

            auto sqe_index = sq_tail & *runtime.sq_ring_mask;
            auto sqes = reinterpret_cast<io_uring_sqe*>(runtime.sqes_ptr);
            auto& sqe = sqes[sqe_index];
            std::memset(&sqe, 0, sizeof(sqe));

            if (endpoint.sink_type == SinkType::unix_dgram && endpoint.unix_dgram_addr_len == 0)
                return false;

            auto user_data = runtime.next_user_data++;
            auto payload_size = line.size();
            runtime.inflight.push_back(
                    sqpoll_inflight{
                            .user_data = user_data,
                            .expected_size = payload_size,
                            .payload = std::string{line},
                    });

            auto& inflight = runtime.inflight.back();
            sqe.fd = endpoint.fd;
            sqe.user_data = user_data;

            if (endpoint.sink_type == SinkType::unix_dgram) {
                inflight.dgram_addr = endpoint.unix_dgram_addr;
                inflight.dgram_iov.iov_base = inflight.payload.data();
                inflight.dgram_iov.iov_len = inflight.payload.size();
                std::memset(&inflight.dgram_msg, 0, sizeof(inflight.dgram_msg));
                inflight.dgram_msg.msg_name = &inflight.dgram_addr;
                inflight.dgram_msg.msg_namelen = endpoint.unix_dgram_addr_len;
                inflight.dgram_msg.msg_iov = &inflight.dgram_iov;
                inflight.dgram_msg.msg_iovlen = 1;

                sqe.opcode = IORING_OP_SENDMSG;
                sqe.addr = reinterpret_cast<uint64_t>(&inflight.dgram_msg);
                sqe.len = 1;
                sqe.msg_flags = MSG_DONTWAIT;
            }
            else {
                sqe.opcode = IORING_OP_WRITE;
                sqe.addr = reinterpret_cast<uint64_t>(inflight.payload.data());
                sqe.len = static_cast<uint32_t>(inflight.payload.size());
                sqe.off = std::numeric_limits<uint64_t>::max();
            }

            runtime.sq_array[sqe_index] = sqe_index;
            std::atomic_ref<uint32_t>(*runtime.sq_tail).store(sq_tail + 1, std::memory_order_release);

            wake_sqpoll(runtime);

            return true;
        }
#endif
    }  // namespace detail

    void sqpoll_runtime_start(sqpoll_runtime& runtime, uint32_t queue_depth) {
        if (queue_depth == 0)
            throw std::invalid_argument{"sqpoll_live requires sqpoll_queue_depth > 0"};

        if (runtime.started) {
            if (runtime.queue_depth != queue_depth)
                throw std::invalid_argument{"sqpoll_live backend already started with different sqpoll_queue_depth"};
            return;
        }

#if defined(__linux__)
        auto params = io_uring_params{};
        auto ring_fd = detail::make_ring_fd(queue_depth, params);
        runtime.ring_fd = ring_fd;
        runtime.queue_depth = queue_depth;
        runtime.next_user_data = 1;
        runtime.completion_failures = 0;
        runtime.endpoints.clear();
        runtime.inflight.clear();
        runtime.inflight.reserve(queue_depth);

        try {
            detail::map_ring_state(runtime, params);
            runtime.started = true;
        } catch (...) {
            detail::unmap_ring_state(runtime);
            ::close(runtime.ring_fd);
            runtime.ring_fd = -1;
            runtime.queue_depth = 0;
            throw;
        }
#else
        (void)runtime;
        (void)queue_depth;
        throw std::invalid_argument{"sqpoll_live backend requires linux"};
#endif
    }

    void sqpoll_runtime_stop(sqpoll_runtime& runtime) noexcept {
#if defined(__linux__)
        sqpoll_runtime_flush(runtime);

        for (auto& endpoint : runtime.endpoints) {
            if (endpoint.owns_fd && endpoint.fd >= 0)
                ::close(endpoint.fd);
        }

        detail::unmap_ring_state(runtime);

        if (runtime.ring_fd >= 0)
            ::close(runtime.ring_fd);
#endif

        runtime.endpoints.clear();
        runtime.inflight.clear();
        runtime.ring_fd = -1;
        runtime.queue_depth = 0;
        runtime.started = false;
        runtime.next_user_data = 1;
        runtime.completion_failures = 0;
    }

    void sqpoll_runtime_add_endpoint(
            sqpoll_runtime& runtime,
            SinkType sink_type,
            std::string_view format,
            std::optional<fs::path> filename,
            bool strict_nonblocking,
            std::optional<int> output_fd,
            std::optional<fs::path> unix_dgram_path,
            std::optional<std::string> pattern_override) {
        if (!runtime.started)
            throw std::invalid_argument{"sqpoll_live backend was not started"};

        if (strict_nonblocking && sink_type == SinkType::file)
            throw std::invalid_argument{"strict_nonblocking does not allow file sink"};

        sqpoll_endpoint endpoint{};
        endpoint.sink_type = sink_type;
        endpoint.pattern = pattern_override.value_or(std::string{format});

#if defined(__linux__)
        switch (sink_type) {
            case SinkType::cout:
                endpoint.fd = STDOUT_FILENO;
                break;
            case SinkType::cerr:
                endpoint.fd = STDERR_FILENO;
                break;
            case SinkType::fd:
                if (!output_fd.has_value())
                    throw std::invalid_argument{"fd sink requires output_fd"};
                endpoint.fd = output_fd.value();
                if (strict_nonblocking)
                    detail::set_fd_nonblocking(endpoint.fd);
                break;
            case SinkType::unix_dgram:
                if (!unix_dgram_path.has_value())
                    throw std::invalid_argument{"unix_dgram sink requires unix_dgram_path"};
                endpoint.path = unix_dgram_path->string();
                endpoint.unix_dgram_addr = detail::make_unix_dgram_addr(endpoint.path);
                endpoint.unix_dgram_addr_len =
                        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.path.size() + 1);
                endpoint.fd = detail::open_unix_dgram_fd();
                endpoint.owns_fd = true;
                break;
            case SinkType::file:
                if (!filename.has_value() || filename->empty())
                    throw std::invalid_argument{"file sink requires filename"};
                endpoint.path = filename->string();
                endpoint.fd = detail::open_file_fd(endpoint.path);
                endpoint.owns_fd = true;
                break;
            default:
                throw std::invalid_argument{"unsupported sink type"};
        }

        auto duplicate =
                std::ranges::find_if(runtime.endpoints.begin(), runtime.endpoints.end(), [&endpoint](auto& current) {
                    return detail::same_endpoint(current, endpoint) && current.pattern == endpoint.pattern;
                });
        if (duplicate != runtime.endpoints.end()) {
            if (endpoint.owns_fd && endpoint.fd >= 0)
                ::close(endpoint.fd);
            return;
        }

        runtime.endpoints.push_back(std::move(endpoint));
#else
        (void)sink_type;
        (void)format;
        (void)filename;
        (void)strict_nonblocking;
        (void)output_fd;
        (void)unix_dgram_path;
        (void)endpoint;
        throw std::invalid_argument{"sqpoll_live backend requires linux"};
#endif
    }

    bool sqpoll_runtime_write(
            sqpoll_runtime& runtime, const sqpoll_endpoint& endpoint, std::string_view line) noexcept {
#if defined(__linux__)
        if (!runtime.started || endpoint.fd < 0)
            return false;

        detail::reap_cqes(runtime, runtime.queue_depth);

        try {
            if (detail::try_queue_write(runtime, endpoint, line))
                return true;

            detail::wake_sqpoll(runtime);
            detail::reap_cqes(runtime, runtime.queue_depth);

            return detail::try_queue_write(runtime, endpoint, line);
        } catch (...) {
            return false;
        }
#else
        (void)runtime;
        (void)endpoint;
        (void)line;
        return false;
#endif
    }

    void sqpoll_runtime_reap(sqpoll_runtime& runtime) noexcept {
#if defined(__linux__)
        detail::reap_cqes(runtime, std::numeric_limits<size_t>::max());
#else
        (void)runtime;
#endif
    }

    void sqpoll_runtime_flush(sqpoll_runtime& runtime) noexcept {
#if defined(__linux__)
        if (!runtime.started)
            return;

        detail::reap_cqes(runtime, std::numeric_limits<size_t>::max());
        detail::wake_sqpoll(runtime);

        while (!runtime.inflight.empty()) {
            auto flags = IORING_ENTER_GETEVENTS;
            if (detail::sqpoll_needs_wakeup(runtime))
                flags |= IORING_ENTER_SQ_WAKEUP;

            auto rv = ::syscall(__NR_io_uring_enter, runtime.ring_fd, 0, 1, flags, nullptr, 0);
            if (rv < 0 && errno != EINTR) {
                runtime.completion_failures += runtime.inflight.size();
                runtime.inflight.clear();
                break;
            }

            detail::reap_cqes(runtime, std::numeric_limits<size_t>::max());
        }

        detail::reap_cqes(runtime, std::numeric_limits<size_t>::max());
#else
        (void)runtime;
#endif
    }

    uint64_t sqpoll_runtime_take_completion_failures(sqpoll_runtime& runtime) noexcept {
        auto out = runtime.completion_failures;
        runtime.completion_failures = 0;
        return out;
    }

}  // namespace un::log::backend
