#pragma once

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
        struct source_loc {
            const char* filename;
            int line;
            const char* function;

            constexpr bool operator==(const source_loc&) const = default;
        };

        // Pass-through by design: the basename strip happens on the consumer (%g render, cached
        // per file-name pointer), so producers never scan the path — least of all on calls the
        // level check is about to filter out.
        inline constexpr source_loc sloc(const std::source_location& loc) {
            return source_loc{loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
        }
    }  // namespace detail
}  // namespace un::log
