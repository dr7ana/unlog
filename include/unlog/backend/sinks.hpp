#pragma once

#include "backend.hpp"

#include "unlog/format.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace un::log::backend {

    enum class sink_mode : uint8_t { single_consumer, multi_consumer };

    template <sink_mode Mode = sink_mode::single_consumer>
    class ostream_sink final : public sink {
        static constexpr bool threadsafe{Mode == sink_mode::multi_consumer};

      public:
        explicit ostream_sink(std::ostream& out, bool color = false) : out_{&out}, color_{color} {}

        void write(std::string_view line) override {
            if constexpr (threadsafe) {
                std::lock_guard lock{mutex_};
            }
            (*out_) << line;
        }

        void flush() override {
            if constexpr (threadsafe) {
                std::lock_guard lock{mutex_};
            }
            out_->flush();
        }

      private:
        std::ostream* out_;
        bool color_;
        mutable std::mutex mutex_;
    };

    using ostream_sink_sc = ostream_sink<sink_mode::single_consumer>;
    using ostream_sink_mc = ostream_sink<sink_mode::multi_consumer>;

    template <sink_mode Mode = sink_mode::single_consumer>
    class file_sink final : public sink {
        static constexpr bool threadsafe{Mode == sink_mode::multi_consumer};

      public:
        explicit file_sink(std::string_view path) : file_{std::string{path}, std::ios::out | std::ios::app} {
            if (!file_.is_open()) {
                using namespace un::log::literals;
                throw std::runtime_error{"failed to open log file: {}"_format(path)};
            }
        }

        void write(std::string_view line) override {
            if constexpr (threadsafe) {
                std::lock_guard lock{mutex_};
            }
            file_ << line;
        }

        void flush() override {
            if constexpr (threadsafe) {
                std::lock_guard lock{mutex_};
            }
            file_.flush();
        }

      private:
        std::ofstream file_;
        mutable std::mutex mutex_;
    };

    using file_sink_sc = file_sink<sink_mode::single_consumer>;
    using file_sink_mc = file_sink<sink_mode::multi_consumer>;

    class fd_sink final : public sink {
      public:
        explicit fd_sink(int fd) : fd_{fd} {
            if (fd_ < 0)
                throw std::invalid_argument{"fd sink requires non-negative fd"};
        }

        void write(std::string_view line) override {
            size_t off = 0;
            while (off < line.size()) {
                auto wrote = ::write(fd_, line.data() + off, line.size() - off);
                if (wrote < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                off += static_cast<size_t>(wrote);
            }
        }

        void flush() override {}

      private:
        int fd_;
    };

    class unix_dgram_sink final : public sink {
      public:
        explicit unix_dgram_sink(std::string_view path) : sock_{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)} {
            if (sock_ < 0)
                throw std::system_error{errno, std::generic_category(), "socket(AF_UNIX, SOCK_DGRAM) failed"};

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;

            if (path.size() >= sizeof(addr.sun_path)) {
                ::close(sock_);
                throw std::invalid_argument{"unix_dgram path is too long"};
            }

            std::ranges::copy(path, addr.sun_path);
            addr.sun_path[path.size()] = '\0';
            if (::connect(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
                auto err = errno;
                ::close(sock_);
                throw std::system_error{err, std::generic_category(), "connect(AF_UNIX) failed"};
            }
        }

        ~unix_dgram_sink() override {
            if (sock_ >= 0)
                ::close(sock_);
        }

        void write(std::string_view line) override { (void)::send(sock_, line.data(), line.size(), MSG_DONTWAIT); }

        void flush() override {}

      private:
        int sock_;
    };

}  // namespace un::log::backend
