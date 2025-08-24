#pragma once

#include <spdlog/async.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <utility>

namespace un::log {
    //

    namespace detail {
        inline constexpr spdlog::source_loc sloc(const std::source_location& loc) {
            std::string_view sv{loc.file_name()};
            if (auto p = sv.rfind('/'); p != sv.npos)
                sv.remove_prefix(p + 1);
            return spdlog::source_loc{sv.data(), static_cast<int>(loc.line()), loc.function_name()};
        }
    }  // namespace detail
}  // namespace un::log
