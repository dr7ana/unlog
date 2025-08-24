#include "unlog.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace un::log {
    using namespace un::log::literals;

    inline auto get_master_sink() {
        static std::shared_ptr<spdlog::sinks::dist_sink_mt> ms;
        if (not ms)
            ms = std::make_shared<spdlog::sinks::dist_sink_mt>();
        return ms;
    }

    std::shared_ptr<spdlog::sinks::dist_sink_mt> master_sink = get_master_sink();

    namespace detail {
        __attribute__((visibility("default"))) std::unordered_map<std::string, logger_ptr>& loggers() {
            static std::unordered_map<std::string, logger_ptr> logger_map;
            return logger_map;
        }

        __attribute__((visibility("default"))) std::shared_ptr<spdlog::details::thread_pool> thread_pool(
                uint8_t thread_count = 1, uint32_t pool_size = 8192) {
            static std::shared_ptr<spdlog::details::thread_pool> tp;
            if (!tp) {
                spdlog::init_thread_pool(pool_size, thread_count);
                tp = spdlog::thread_pool();
            }
            return tp;
        }

        __attribute__((visibility("default"))) const std::shared_ptr<Logger>& default_logger() {
            static std::shared_ptr<Logger> dl;
            if (not dl)
                dl = std::make_shared<Logger>();
            return dl;
        }

        __attribute__((visibility("default"))) const logger_ptr& get_global_logger() {
            return *default_logger();
        }

        __attribute__((visibility("default"))) std::mutex& loggers_mutex() {
            static std::mutex loggers_mutex_impl;
            return loggers_mutex_impl;
        }

        __attribute__((visibility("default"))) LogLevel& default_log_level() {
            static LogLevel level_default = LogLevel::info;
            return level_default;
        }

        LogLevel get_default_level() {
            return default_log_level();
        }

        void for_each_logger(std::function<void(logger_ptr&)> hook) {
            if (hook) {
                std::lock_guard lock{detail::loggers_mutex()};

                for (auto& [_, logger] : loggers())
                    hook(logger);
            }
        }

        void for_each_sink(std::function<void(sink_ptr&)> hook) {
            if (hook) {
                auto& sinks = master_sink->sinks();

                for (auto& sink : sinks)
                    hook(sink);
            }
        }

        void set_default_level(LogLevel level) {
            default_log_level() = level;
        }

        void reset_level(LogLevel level) {
            set_default_level(level);
            default_logger()->set_level(level);
            for_each_sink([level](sink_ptr& sink) { sink->set_level(level); });
        }
    }  // namespace detail

    const logger_ptr& global_logger() {
        return detail::get_global_logger();
    }

    Logger::Logger(cspan name) :
            config{Config::make_default(name)}, logger_name{config.name.data(), config.name.size()} {
        std::lock_guard lock{detail::loggers_mutex()};
        // hold a spot in the map for the logger by emplacing nullptr
        detail::loggers()[logger_name];
    }

    void Logger::get_logger() {
        if (have_logger)
            return;

        std::lock_guard lock{detail::loggers_mutex()};
        auto& maybe_logger = detail::loggers()[logger_name];

        if (!maybe_logger) {
            maybe_logger = std::make_shared<spdlog::logger>(logger_name, get_master_sink());
            add_sink<spdlog::sinks::stdout_color_sink_mt>(config);
        }

        logger = maybe_logger;
        logger->set_level(detail::default_log_level());
        have_logger = true;
    }

    void Logger::add_sink(const Config& conf, sink_ptr sink) {
        return detail::add_sink(conf, sink);
    }

    void Logger::initialize(const Config& conf) {
        if (conf.cout_log()) {
            if (conf.threadsafe()) {
                if (conf.color())
                    add_sink<spdlog::sinks::stdout_color_sink_mt>(conf);
                else
                    add_sink<spdlog::sinks::stdout_sink_mt>(conf);
            }
            else {
                if (conf.color())
                    add_sink<spdlog::sinks::stdout_color_sink_st>(conf);
                else
                    add_sink<spdlog::sinks::stdout_sink_st>(conf);
            }
        }
        else if (conf.cerr_log()) {
            if (conf.threadsafe()) {
                if (conf.color())
                    add_sink<spdlog::sinks::stderr_color_sink_mt>(conf);
                else
                    add_sink<spdlog::sinks::stderr_sink_mt>(conf);
            }
            else {
                if (conf.color())
                    add_sink<spdlog::sinks::stderr_color_sink_st>(conf);
                else
                    add_sink<spdlog::sinks::stderr_sink_st>(conf);
            }
        }
        else if (conf.file_log()) {
            if (conf.threadsafe())
                add_sink<spdlog::sinks::basic_file_sink_mt>(conf, conf.file());
            else
                add_sink<spdlog::sinks::basic_file_sink_st>(conf, conf.file());
        }
        else
            throw std::runtime_error{"Invalid config created: {}"_format(conf)};
    }

    void Logger::set_level(LogLevel level) {
        logger->set_level(level);
    }

    void Logger::make_logger(const Config& conf, bool make_default) {
        std::lock_guard lock{detail::loggers_mutex()};
        auto& maybe_logger = detail::loggers()[{conf.name.data(), conf.name.size()}];

        if (maybe_logger != nullptr)
            throw std::invalid_argument{"A logger with the name {} already exists"_format(conf.name)};

        if (conf.async()) {
            maybe_logger = std::make_shared<spdlog::async_logger>(
                    logger_name, get_master_sink(), detail::thread_pool(conf.threads, conf.pool_threads));
        }
        else {
            maybe_logger = std::make_shared<spdlog::logger>(logger_name, get_master_sink());
        }

        initialize(conf);

        // initialize logger w/ pattern
        if (make_default) {
            logger = maybe_logger;
            config = conf;
            logger_name.assign(config.name.data(), config.name.size());
        }
    }

    void flush() {
        master_sink->flush();
    }

}  // namespace un::log
