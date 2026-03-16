#pragma once

#include "unlog/config.hpp"
#include "unlog/utils.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace un::log::backend {

    struct producer_stats {
        uint64_t emitted{0};
        uint64_t dropped{0};
        uint64_t truncated{0};

        constexpr bool operator==(const producer_stats& obj) const {
            return emitted == obj.emitted && dropped == obj.dropped && truncated == obj.truncated;
        }
    };

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

}  // namespace un::log::backend
