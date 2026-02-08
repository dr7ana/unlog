#pragma once

#include "producer.hpp"
#include "sqpoll.hpp"

#include "unlog/config.hpp"
#include "unlog/utils.hpp"

#include <atomic>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace un::log::test {
    struct test_helper;
}

namespace un::log::backend {

    struct log_entry {
        std::string logger_name;
        log_level level{log_level::info};
        detail::source_loc source_location{};
        std::string message;
        uint64_t timestamp{0};
    };

    class sink {
      public:
        virtual ~sink() = default;
        virtual void write(std::string_view line) = 0;
        virtual void flush() = 0;
        virtual bool supports_color() const { return false; }
    };

    using sink_ptr = std::shared_ptr<sink>;

    struct sink_entry {
        sink_ptr sink;
        std::string pattern;
    };

    template <typename T>
    concept sink_t = std::derived_from<T, sink>;

    class sqpoll_backend final {
        friend struct un::log::test::test_helper;

      public:
        ~sqpoll_backend();

        template <detail::basic_config_type Conf>
        void init(const Conf& conf) {
            init(detail::config_sink_type(conf),
                 conf.format,
                 detail::config_filename(conf),
                 detail::config_clock_type_v<Conf>,
                 detail::config_strict_nonblocking(conf),
                 detail::config_sqpoll_queue_depth(conf),
                 detail::config_output_fd(conf),
                 detail::config_unix_dgram_path(conf));
        }

        void init(
                SinkType sink_type,
                std::string_view format,
                std::optional<fs::path> filename,
                ClockType timestamp_mode,
                bool strict_nonblocking,
                size_t sqpoll_queue_depth,
                std::optional<int> output_fd,
                std::optional<fs::path> unix_dgram_path);
        void add_sink(sink_ptr sink_obj, std::optional<std::string> pattern);
        void log(log_entry&& rec);
        void flush();
        producer_stats stats_snapshot() const;

      private:
        mutable std::shared_mutex mutex_;
        sqpoll_runtime runtime_{};
        std::vector<sink_entry> custom_sinks_;
        std::atomic<ClockType> clock_type_{ClockType::steady};
        std::atomic<uint64_t> emitted_{0};
        std::atomic<uint64_t> dropped_{0};
        std::atomic<uint64_t> truncated_{0};
    };

}  // namespace un::log::backend
