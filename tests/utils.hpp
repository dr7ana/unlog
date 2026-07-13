#pragma once

#include "unlog.hpp"

#include "unlog/backend/sinks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace un::log::test {

    using namespace un::log::literals;
    using test_log = configured<default_global_config>;

    void get_runtime_backend(const std::function<void()>& fn);
    bool consumer_thread_started();
    void reset_runtime_for_test();
    size_t threadsafe_producer_count();

    struct test_helper {
        template <typename... Opt>
            requires detail::valid_channel_opt_pack<Opt...>
        static auto make_config(
                std::string_view name = "unlog"sv,
                SinkType sink_type = SinkType::cout,
                uint8_t flags = 0,
                std::optional<fs::path> filename = std::nullopt,
                std::optional<int> output_fd = std::nullopt,
                std::optional<fs::path> unix_dgram_path = std::nullopt) {
            return config<Opt...>{
                    name,
                    sink_type,
                    flags,
                    std::move(filename),
                    std::move(output_fd),
                    std::move(unix_dgram_path),
            };
        }

        static void reset_runtime_state() { reset_runtime_for_test(); }

        static size_t producer_count() { return threadsafe_producer_count(); }
    };

    struct runtime_state_guard {
        runtime_state_guard() { test_helper::reset_runtime_state(); }

        ~runtime_state_guard() { test_helper::reset_runtime_state(); }
    };

    class socket_wrapper {
      public:
        socket_wrapper() = default;
        explicit socket_wrapper(int fd) noexcept : fd_{fd} {}

        ~socket_wrapper() { reset(); }

        socket_wrapper(const socket_wrapper&) = delete;
        socket_wrapper& operator=(const socket_wrapper&) = delete;

        socket_wrapper(socket_wrapper&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

        socket_wrapper& operator=(socket_wrapper&& other) noexcept {
            if (this != &other) {
                reset(std::exchange(other.fd_, -1));
            }
            return *this;
        }

        [[nodiscard]] static socket_wrapper null_sink() {
            auto fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (fd < 0) {
                throw std::system_error{errno, std::generic_category(), "open /dev/null"};
            }
            return socket_wrapper{fd};
        }

        [[nodiscard]] static std::array<socket_wrapper, 2> make_pair() {
            int sockets[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
                throw std::system_error{errno, std::generic_category(), "socketpair"};
            }
            return {socket_wrapper{sockets[0]}, socket_wrapper{sockets[1]}};
        }

        [[nodiscard]] int get() const noexcept { return fd_; }
        [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

        [[nodiscard]] std::string read_available(size_t capacity = 4096u) const {
            auto output = std::string(capacity, '\0');
            auto size = ::recv(fd_, output.data(), output.size(), MSG_DONTWAIT);
            if (size < 0) {
                throw std::system_error{errno, std::generic_category(), "recv socket"};
            }
            output.resize(static_cast<size_t>(size));
            return output;
        }

        void reset(int replacement = -1) noexcept {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = replacement;
        }

      private:
        int fd_{-1};
    };

    struct util {
        static std::stringstream stream;
        static std::mutex stream_mutex;

        struct capture_sink final : backend::sink {
            void write(std::string_view line) override {
                std::lock_guard lock{util::stream_mutex};
                util::stream << line;
            }

            void flush() override {
                std::lock_guard lock{util::stream_mutex};
                util::stream.flush();
            }
        };

        template <global_config Global = default_global_config>
        static auto reset() {
            configured<Global>::flush();
            std::lock_guard lock{stream_mutex};
            stream = {};
            stream.clear();
        }

        template <global_config Global = default_global_config>
        static auto capture_test_logs(log_level level = configured<Global>::get_global_level()) {
            reset<Global>();
            configured<Global>::set_global_level(level);
            auto conf = config<>::make("capture");
            detail::add_sink<Global>(conf, std::make_shared<capture_sink>());
        }

        template <global_config Global = default_global_config, detail::basic_config_type Conf>
        static auto capture_test_logs(const Conf& conf, log_level level = configured<Global>::get_global_level()) {
            reset<Global>();
            configured<Global>::set_global_level(level);
            detail::add_sink<Global>(conf, std::make_shared<capture_sink>());
        }

        template <global_config Global = default_global_config>
        static std::string captured_output() {
            configured<Global>::flush();
            std::lock_guard lock{stream_mutex};
            return stream.str();
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_CONTAINS {
        REQUIRE_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_CONTAINS {
        CHECK_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.contains(msg));
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct REQUIRE_NOT_CONTAINS {
        REQUIRE_NOT_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.contains(msg) == false);
        }
    };

    template <typename T, typename U = std::remove_cvref_t<T>>
        requires std::convertible_to<U, std::string_view>
    struct CHECK_NOT_CONTAINS {
        CHECK_NOT_CONTAINS(U msg, const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.contains(msg) == false);
        }
    };

    struct REQUIRE_EMPTY {
        REQUIRE_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            REQUIRE(output.empty());
        }
    };

    struct CHECK_EMPTY {
        CHECK_EMPTY(const std::source_location& source_location = std::source_location::current()) {
            auto output = util::captured_output();
            INFO("Location: {}:{}"_format(source_location.file_name(), source_location.line()));
            INFO("Contents: " << output);
            CHECK(output.empty());
        }
    };

    // deduction guides
    template <typename T>
    REQUIRE_CONTAINS(T msg) -> REQUIRE_CONTAINS<T>;
    template <typename T>
    CHECK_CONTAINS(T msg) -> CHECK_CONTAINS<T>;
    template <typename T>
    REQUIRE_NOT_CONTAINS(T msg) -> REQUIRE_NOT_CONTAINS<T>;
    template <typename T>
    CHECK_NOT_CONTAINS(T msg) -> CHECK_NOT_CONTAINS<T>;

}  // namespace un::log::test
