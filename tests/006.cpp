#include "utils.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace un::log::test {

    static std::filesystem::path temp_artifact_path(std::string_view stem, std::string_view ext) {
        static std::atomic_uint64_t counter{0};
        auto base = std::filesystem::temp_directory_path();
        auto tick = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        auto id = counter.fetch_add(1, std::memory_order_relaxed);
        return base / "unlog-{}-{}-{}{}"_format(stem, tick, id, ext);
    }

    static uint64_t steady_now_ns() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
    }

    /*
        Pseudo fixtures for backend-path testing:
        - counting_sink provides deterministic write/flush accounting without touching stdout/files.
        - fake_sq_runtime wires a minimal in-memory SQ view so tests can assert opcode selection,
          queue-full behavior, and retry/flush control flow without depending on kernel SQPOLL state.
        These keep tests fast, isolated, and stable while still exercising real submission code paths.
    */
    struct counting_sink final : backend::sink {
        void write(std::string_view line) override {
            ++write_calls;
            written_bytes += line.size();
        }

        void flush() override { ++flush_calls; }

        uint64_t write_calls{0};
        uint64_t flush_calls{0};
        size_t written_bytes{0};
    };

#if defined(__linux__)
    struct fake_sq_runtime {
        uint32_t sq_head{0};
        uint32_t sq_tail{0};
        uint32_t sq_ring_mask{0};
        uint32_t sq_ring_entries{1};
        uint32_t sq_flags{0};
        uint32_t sq_array[1]{0};
        io_uring_sqe sqes[1]{};
        backend::sqpoll_runtime runtime{};

        fake_sq_runtime() {
            runtime.started = true;
            runtime.queue_depth = 1;
            runtime.ring_fd = -1;
            runtime.sq_head = &sq_head;
            runtime.sq_tail = &sq_tail;
            runtime.sq_ring_mask = &sq_ring_mask;
            runtime.sq_ring_entries = &sq_ring_entries;
            runtime.sq_flags = &sq_flags;
            runtime.sq_array = sq_array;
            runtime.sqes_ptr = sqes;
        }
    };

    struct unix_dgram_receiver {
        int fd{-1};
        std::filesystem::path path;

        ~unix_dgram_receiver() {
            if (fd >= 0)
                ::close(fd);
            if (!path.empty())
                std::filesystem::remove(path);
        }
    };
#endif

    TEST_CASE("006 - live backend rejects zero sqpoll queue depth", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        auto cfg = config<>::make_sqpoll("sqpoll-zero-depth");
        cfg.sqpoll_queue_depth = 0;

        CHECK_THROWS_AS(live.init(cfg), std::invalid_argument);
    }

    TEST_CASE("006 - strict nonblocking rejects file sink in live backend", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        auto path = temp_artifact_path("strict-file", ".log");

        CHECK_THROWS_AS(
                live.init(
                        SinkType::file,
                        DEFAULT_PATTERN,
                        std::optional<fs::path>{path},
                        ClockType::steady,
                        true,
                        options::default_sqpoll_queue_depth,
                        std::nullopt,
                        std::nullopt),
                std::invalid_argument);
    }

    TEST_CASE("006 - sqpoll runtime failure counter accessor resets", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.completion_failures = 3;

        CHECK(backend::sqpoll_runtime_take_completion_failures(runtime) == 3u);
        CHECK(backend::sqpoll_runtime_take_completion_failures(runtime) == 0u);
    }

    TEST_CASE("006 - sqpoll endpoint rejects missing output fd", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        CHECK_THROWS_AS(
                backend::sqpoll_runtime_add_endpoint(
                        runtime,
                        SinkType::fd,
                        DEFAULT_PATTERN,
                        std::nullopt,
                        true,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt),
                std::invalid_argument);
    }

    TEST_CASE("006 - sqpoll endpoint rejects missing unix dgram path", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        CHECK_THROWS_AS(
                backend::sqpoll_runtime_add_endpoint(
                        runtime,
                        SinkType::unix_dgram,
                        DEFAULT_PATTERN,
                        std::nullopt,
                        true,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt),
                std::invalid_argument);
    }

    TEST_CASE("006 - live backend counts enqueue failures per endpoint", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        std::stringstream stream;
        live.add_sink(std::make_shared<backend::ostream_sink>(stream), std::nullopt);

        test_helper::add_live_endpoint(
                live,
                backend::sqpoll_endpoint{
                        .sink_type = SinkType::fd,
                        .fd = 10,
                        .owns_fd = false,
                        .path = "",
                        .pattern = DEFAULT_PATTERN,
                });
        test_helper::add_live_endpoint(
                live,
                backend::sqpoll_endpoint{
                        .sink_type = SinkType::fd,
                        .fd = 11,
                        .owns_fd = false,
                        .path = "",
                        .pattern = DEFAULT_PATTERN,
                });

        live.log(
                backend::log_entry{
                        .logger_name = "enqueue-count",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 50, "enqueue_count"},
                        .message = "enqueue-failure-count",
                        .timestamp = steady_now_ns(),
                });

        auto stats = live.stats_snapshot();
        CHECK(stats.emitted == 1u);
        CHECK(stats.dropped == 2u);
        CHECK(stats.truncated == 0u);
    }

    TEST_CASE("006 - live backend maps completion failures into dropped once", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        std::stringstream stream;
        live.add_sink(std::make_shared<backend::ostream_sink>(stream), std::nullopt);

        test_helper::set_live_completion_failures(live, 3);
        live.log(
                backend::log_entry{
                        .logger_name = "completion-map",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 79, "completion_map"},
                        .message = "completion-failure-map",
                        .timestamp = steady_now_ns(),
                });

        auto after_first = live.stats_snapshot();
        CHECK(after_first.emitted == 1u);
        CHECK(after_first.dropped == 3u);
        CHECK(after_first.truncated == 0u);

        live.log(
                backend::log_entry{
                        .logger_name = "completion-map",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 93, "completion_map"},
                        .message = "completion-failure-consumed",
                        .timestamp = steady_now_ns(),
                });

        auto after_second = live.stats_snapshot();
        CHECK(after_second.emitted == 2u);
        CHECK(after_second.dropped == 3u);
        CHECK(after_second.truncated == 0u);
    }

    TEST_CASE("006 - sqpoll file endpoint allows best-effort mode", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        auto path = temp_artifact_path("best-effort", ".log");
        std::filesystem::remove(path);

        backend::sqpoll_runtime_add_endpoint(
                runtime,
                SinkType::file,
                DEFAULT_PATTERN,
                std::optional<fs::path>{path},
                false,
                std::nullopt,
                std::nullopt,
                std::nullopt);
        REQUIRE(runtime.endpoints.size() == 1u);
        CHECK(runtime.endpoints[0].sink_type == SinkType::file);
        CHECK(runtime.endpoints[0].owns_fd == true);
        CHECK(runtime.endpoints[0].fd >= 0);
        CHECK(runtime.endpoints[0].path == path.string());

        backend::sqpoll_runtime_add_endpoint(
                runtime,
                SinkType::file,
                DEFAULT_PATTERN,
                std::optional<fs::path>{path},
                false,
                std::nullopt,
                std::nullopt,
                std::nullopt);
        CHECK(runtime.endpoints.size() == 1u);

        backend::sqpoll_runtime_stop(runtime);
        std::filesystem::remove(path);
    }

    TEST_CASE("006 - sqpoll unix_dgram endpoint caches destination metadata", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        auto path = temp_artifact_path("unix-dgram", ".sock");
        auto cfg = test_helper::make_sqpoll_config(
                "unix-dgram", SinkType::unix_dgram, 0, std::nullopt, std::nullopt, path);

        backend::sqpoll_runtime_add_endpoint(runtime, cfg, std::nullopt);
        REQUIRE(runtime.endpoints.size() == 1u);
        CHECK(runtime.endpoints[0].sink_type == SinkType::unix_dgram);
        CHECK(runtime.endpoints[0].owns_fd == true);
        CHECK(runtime.endpoints[0].fd >= 0);
        CHECK(runtime.endpoints[0].path == path.string());
        CHECK(runtime.endpoints[0].unix_dgram_addr_len > 0);

        backend::sqpoll_runtime_add_endpoint(runtime, cfg, std::nullopt);
        CHECK(runtime.endpoints.size() == 1u);

        backend::sqpoll_runtime_stop(runtime);
    }

    TEST_CASE("006 - live backend flush flushes custom sinks", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        auto sink = std::make_shared<counting_sink>();
        live.add_sink(sink, std::nullopt);

        live.log(
                backend::log_entry{
                        .logger_name = "flush-sink",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 200, "flush_sink"},
                        .message = "flush-message",
                        .timestamp = steady_now_ns(),
                });

        auto before_flush = live.stats_snapshot();
        CHECK(before_flush.emitted == 1u);
        CHECK(before_flush.dropped == 0u);
        CHECK(before_flush.truncated == 0u);
        CHECK(sink->write_calls == 2u);
        CHECK(sink->written_bytes > 0u);

        live.flush();
        CHECK(sink->flush_calls == 1u);
    }

    TEST_CASE("006 - live backend uses configured clock type for elapsed rendering", "[006][backend][sqpoll][clock]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        std::stringstream stream;
        live.add_sink(std::make_shared<backend::ostream_sink>(stream), "[%*] %v");

        auto now_system = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        std::chrono::system_clock::now().time_since_epoch())
                                                        .count());

        test_helper::set_live_clock_type(live, ClockType::system);
        live.log(
                backend::log_entry{
                        .logger_name = "clock-mode",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 256, "clock_mode"},
                        .message = "clock-mode-message",
                        .timestamp = now_system,
                });
        auto system_line = stream.str();
        REQUIRE(system_line.contains("clock-mode-message"));

        stream.str("");
        stream.clear();

        test_helper::set_live_clock_type(live, ClockType::steady);
        live.log(
                backend::log_entry{
                        .logger_name = "clock-mode",
                        .level = log_level::info,
                        .source_location = detail::source_loc{"006.cpp", 270, "clock_mode"},
                        .message = "clock-mode-message",
                        .timestamp = now_system,
                });
        auto steady_line = stream.str();
        REQUIRE(steady_line.contains("clock-mode-message"));

        CHECK(test_helper::live_clock_type(live) == ClockType::steady);
        CHECK(system_line != steady_line);
    }

#if defined(__linux__)
    TEST_CASE("006 - sqpoll write uses write opcode and drops when ring is full", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto fake = fake_sq_runtime{};
        auto endpoint = backend::sqpoll_endpoint{
                .sink_type = SinkType::fd,
                .fd = 9,
                .owns_fd = false,
                .path = "",
                .pattern = DEFAULT_PATTERN,
        };

        CHECK(backend::sqpoll_runtime_write(fake.runtime, endpoint, "first"));
        CHECK(fake.sq_tail == 1u);
        CHECK(fake.sqes[0].opcode == IORING_OP_WRITE);
        CHECK(fake.runtime.inflight.size() == 1u);

        CHECK_FALSE(backend::sqpoll_runtime_write(fake.runtime, endpoint, "second"));
        CHECK(fake.sq_tail == 1u);
        CHECK(fake.runtime.inflight.size() == 1u);
    }

    TEST_CASE(
            "006 - sqpoll write with invalid unix dgram endpoint does not enqueue inflight", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto fake = fake_sq_runtime{};
        auto endpoint = backend::sqpoll_endpoint{
                .sink_type = SinkType::unix_dgram,
                .fd = 9,
                .owns_fd = false,
                .path = "/tmp/unlog.sock",
                .pattern = DEFAULT_PATTERN,
        };
        endpoint.unix_dgram_addr_len = 0;

        CHECK_FALSE(backend::sqpoll_runtime_write(fake.runtime, endpoint, "abc"));
        CHECK(fake.sq_tail == 0u);
        CHECK(fake.runtime.inflight.empty());
    }

    TEST_CASE("006 - sqpoll write uses sendmsg opcode for unix dgram", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto fake = fake_sq_runtime{};
        auto endpoint = backend::sqpoll_endpoint{
                .sink_type = SinkType::unix_dgram,
                .fd = 9,
                .owns_fd = false,
                .path = "/tmp/unlog.sock",
                .pattern = DEFAULT_PATTERN,
        };
        endpoint.unix_dgram_addr.sun_family = AF_UNIX;
        auto name = "/tmp/unlog.sock";
        std::memcpy(endpoint.unix_dgram_addr.sun_path, name, std::strlen(name) + 1);
        endpoint.unix_dgram_addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(name) + 1);

        CHECK(backend::sqpoll_runtime_write(fake.runtime, endpoint, "abc"));
        CHECK(fake.sq_tail == 1u);
        CHECK(fake.sqes[0].opcode == IORING_OP_SENDMSG);
        CHECK(fake.sqes[0].msg_flags == MSG_DONTWAIT);
        CHECK(fake.runtime.inflight.size() == 1u);

        auto* msg = reinterpret_cast<msghdr*>(fake.sqes[0].addr);
        REQUIRE(msg != nullptr);
        CHECK(msg->msg_namelen == endpoint.unix_dgram_addr_len);
        CHECK(msg->msg_iovlen == 1u);
    }

    TEST_CASE("006 - endpoint dedupe is pattern-sensitive", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        auto cfg = test_helper::make_sqpoll_config("pattern-dedupe", SinkType::fd, 0, std::nullopt, 1);
        cfg.strict_nonblocking = false;

        auto pattern_a = std::optional<std::string>{"[%n] %v"};
        auto pattern_b = std::optional<std::string>{"[%l] %v"};

        backend::sqpoll_runtime_add_endpoint(runtime, cfg, pattern_a);
        CHECK(runtime.endpoints.size() == 1u);

        backend::sqpoll_runtime_add_endpoint(runtime, cfg, pattern_a);
        CHECK(runtime.endpoints.size() == 1u);

        backend::sqpoll_runtime_add_endpoint(runtime, cfg, pattern_b);
        CHECK(runtime.endpoints.size() == 2u);
    }

    TEST_CASE("006 - unix dgram oversize path throws without mutating endpoints", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;

        auto fd_cfg = test_helper::make_sqpoll_config("baseline-endpoint", SinkType::fd, 0, std::nullopt, 1);
        fd_cfg.strict_nonblocking = false;

        backend::sqpoll_runtime_add_endpoint(runtime, fd_cfg, std::nullopt);
        REQUIRE(runtime.endpoints.size() == 1u);

        auto oversize_path = std::string(sizeof(sockaddr_un{}.sun_path), 'x');
        auto dgram_cfg = test_helper::make_sqpoll_config(
                "oversize-unix-path", SinkType::unix_dgram, 0, std::nullopt, std::nullopt, fs::path{oversize_path});

        CHECK_THROWS_AS(backend::sqpoll_runtime_add_endpoint(runtime, dgram_cfg, std::nullopt), std::invalid_argument);
        CHECK(runtime.endpoints.size() == 1u);
        CHECK(runtime.endpoints[0].sink_type == SinkType::fd);
    }

    TEST_CASE("006 - sqpoll flush clears inflight on enter failure", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto runtime = backend::sqpoll_runtime{};
        runtime.started = true;
        runtime.ring_fd = -1;
        runtime.inflight.push_back(
                backend::sqpoll_inflight{
                        .user_data = 7,
                        .expected_size = 3,
                        .payload = "abc",
                });

        backend::sqpoll_runtime_flush(runtime);
        CHECK(runtime.inflight.empty());
        CHECK(backend::sqpoll_runtime_take_completion_failures(runtime) == 1u);
    }

    TEST_CASE("006 - sqpoll unix dgram delivers to real receiver", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        auto receiver = unix_dgram_receiver{};
        receiver.path = temp_artifact_path("receiver", ".sock");
        std::filesystem::remove(receiver.path);

        receiver.fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        REQUIRE(receiver.fd >= 0);

        auto addr = sockaddr_un{};
        addr.sun_family = AF_UNIX;
        auto path_str = receiver.path.string();
        REQUIRE(path_str.size() < sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, path_str.c_str(), path_str.size() + 1);
        auto addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_str.size() + 1);
        if (::bind(receiver.fd, reinterpret_cast<const sockaddr*>(&addr), addr_len) != 0) {
            INFO("bind failed: errno={} ({})"_format(errno, std::strerror(errno)));
            SKIP("unix dgram bind unavailable in this environment");
        }

        auto timeout = timeval{1, 0};
        REQUIRE(::setsockopt(receiver.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

        auto cfg = test_helper::make_sqpoll_config(
                "dgram-integration", SinkType::unix_dgram, 0, std::nullopt, std::nullopt, receiver.path);
        cfg.sqpoll_queue_depth = 64;

        try {
            make_logger(cfg, true);
        } catch (const std::exception& ex) {
            INFO("sqpoll unavailable: " << ex.what());
            SKIP("sqpoll unavailable in this environment");
        }

        auto message = "dgram-integration-message";
        unlog::info("{}", message);
        unlog::flush();

        char buffer[4096]{};
        auto nread = ::recv(receiver.fd, buffer, sizeof(buffer), 0);
        REQUIRE(nread > 0);

        auto received = std::string{buffer, static_cast<size_t>(nread)};
        CHECK(received.contains(message));
    }
#endif

    TEST_CASE("006 - live backend flush maps pending completion failures to dropped", "[006][backend][sqpoll]") {
        runtime_state_guard guard;

        backend::sqpoll_backend live;
        test_helper::set_live_completion_failures(live, 2);

        live.flush();

        auto stats = live.stats_snapshot();
        CHECK(stats.emitted == 0u);
        CHECK(stats.dropped == 2u);
        CHECK(stats.truncated == 0u);
    }

}  // namespace un::log::test
