#pragma once

#include "unlog/config.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace un::log::backend {

    struct sqpoll_endpoint {
        SinkType sink_type{SinkType::cout};
        int fd{-1};
        bool owns_fd{false};
        std::string path;
        std::string pattern;
#if defined(__linux__)
        sockaddr_un unix_dgram_addr{};
        socklen_t unix_dgram_addr_len{0};
#endif
    };

    struct sqpoll_inflight {
        uint64_t user_data{0};
        size_t expected_size{0};
        std::string payload;
#if defined(__linux__)
        iovec dgram_iov{};
        msghdr dgram_msg{};
        sockaddr_un dgram_addr{};
#endif
    };

    struct sqpoll_runtime {
        int ring_fd{-1};
        uint32_t queue_depth{0};
        bool started{false};
        void* sq_ring_ptr{nullptr};
        size_t sq_ring_size{0};
        void* cq_ring_ptr{nullptr};
        size_t cq_ring_size{0};
        void* sqes_ptr{nullptr};
        size_t sqes_size{0};
        uint32_t* sq_head{nullptr};
        uint32_t* sq_tail{nullptr};
        uint32_t* sq_ring_mask{nullptr};
        uint32_t* sq_ring_entries{nullptr};
        uint32_t* sq_flags{nullptr};
        uint32_t* sq_dropped{nullptr};
        uint32_t* sq_array{nullptr};
        uint32_t* cq_head{nullptr};
        uint32_t* cq_tail{nullptr};
        uint32_t* cq_ring_mask{nullptr};
        uint32_t* cq_ring_entries{nullptr};
        void* cqes{nullptr};
        uint64_t next_user_data{1};
        uint64_t completion_failures{0};
        std::vector<sqpoll_endpoint> endpoints;
        std::vector<sqpoll_inflight> inflight;
    };

    void sqpoll_runtime_start(sqpoll_runtime& runtime, uint32_t queue_depth);
    void sqpoll_runtime_stop(sqpoll_runtime& runtime) noexcept;

    void sqpoll_runtime_add_endpoint(
            sqpoll_runtime& runtime,
            SinkType sink_type,
            std::string_view format,
            std::optional<fs::path> filename,
            bool strict_nonblocking,
            std::optional<int> output_fd,
            std::optional<fs::path> unix_dgram_path,
            std::optional<std::string> pattern_override = std::nullopt);

    template <detail::basic_config_type Conf>
    void sqpoll_runtime_add_endpoint(
            sqpoll_runtime& runtime, const Conf& conf, std::optional<std::string> pattern_override = std::nullopt) {
        sqpoll_runtime_add_endpoint(
                runtime,
                detail::config_sink_type(conf),
                conf.format,
                detail::config_filename(conf),
                detail::config_strict_nonblocking(conf),
                detail::config_output_fd(conf),
                detail::config_unix_dgram_path(conf),
                std::move(pattern_override));
    }

    bool sqpoll_runtime_write(sqpoll_runtime& runtime, const sqpoll_endpoint& endpoint, std::string_view line) noexcept;
    void sqpoll_runtime_reap(sqpoll_runtime& runtime) noexcept;
    void sqpoll_runtime_flush(sqpoll_runtime& runtime) noexcept;
    uint64_t sqpoll_runtime_take_completion_failures(sqpoll_runtime& runtime) noexcept;

}  // namespace un::log::backend
