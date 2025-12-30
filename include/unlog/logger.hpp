#pragma once

#include "config.hpp"
#include "format.hpp"

namespace un::log {

    using logger_ptr = std::shared_ptr<spdlog::logger>;
    using sink_ptr = spdlog::sink_ptr;

    template <typename T>
    concept spdlog_sink_t = std::derived_from<T, spdlog::sinks::sink>;

    struct Logger {
      private:
        std::atomic<bool> have_logger{false};
        logger_ptr logger;
        Config config;
        std::string logger_name;

        void get_logger();

        void add_sink(const Config& conf, sink_ptr sink);

        void set_sinks(const Config& conf, sink_ptr sink);

        template <spdlog_sink_t T, typename... Arg>
        void add_sink(const Config& conf, Arg... args) {
            return add_sink(conf, std::make_shared<T>(std::forward<Arg>(args)...));
        }

        template <spdlog_sink_t T, typename... Arg>
        void set_sinks(const Config& conf, Arg... args) {
            return set_sinks(conf, std::make_shared<T>(std::forward<Arg>(args)...));
        }

        void initialize(const Config& conf, bool make_default);

      public:
        explicit Logger(std::string_view name = "unlog"sv);

        void set_level(LogLevel level);

        void make_logger(const Config& conf, bool make_default = false);

        operator logger_ptr&() {
            if (have_logger)
                return logger;
            get_logger();
            return logger;
        }

        std::string name() const { return logger_name; }

        // Operators that access the underlying logger, creating it if necessary
        spdlog::logger& operator*() { return *static_cast<const logger_ptr&>(*this); }
        spdlog::logger* operator->() { return static_cast<const logger_ptr&>(*this).get(); }
    };

    // Global default logger
    const logger_ptr& global_logger();
    // Global distribution sink multiplexer: holds a vector of sink
    extern std::shared_ptr<spdlog::sinks::dist_sink_mt> master_sink;

    namespace detail {
        LogLevel get_default_level();

        void set_default_level(LogLevel level);

        void make_logger(const Config& conf, bool make_default);

        void add_sink(sink_ptr sink);

        void add_sink(const Config& conf, sink_ptr sink);

        void set_sinks(const Config& conf, sink_ptr sink);
    }  // namespace detail
}  // namespace un::log
