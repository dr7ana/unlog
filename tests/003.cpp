#include "utils.hpp"

#include <array>
#include <string>

using namespace un::log::literals;

namespace un::log::test {

    TEST_CASE("003 - fmt formatting for cspan", "[003][format][span]") {
        auto sp = "hello"_sp;
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "['h', 'e', 'l', 'l', 'o']");
    }

    TEST_CASE("003 - fmt formatting for uspan", "[003][format][span]") {
        auto sp = "world"_usp;
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "[119, 111, 114, 108, 100]");
    }

    TEST_CASE("003 - fmt formatting for bspan", "[003][format][span]") {
        auto sp = "bytes"_bsp;
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "[98, 121, 116, 101, 115]");
    }

    TEST_CASE("003 - fmt formatting for runtime cspan", "[003][format][span]") {
        std::string value = "hello";
        cspan sp{value.data(), value.size()};
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "['h', 'e', 'l', 'l', 'o']");
    }

    TEST_CASE("003 - fmt formatting for runtime uspan", "[003][format][span]") {
        std::array<unsigned char, 5> value{'w', 'o', 'r', 'l', 'd'};
        uspan sp{value.data(), value.size()};
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "[119, 111, 114, 108, 100]");
    }

    TEST_CASE("003 - fmt formatting for runtime bspan", "[003][format][span]") {
        std::array<std::byte, 5> value{
                static_cast<std::byte>('b'),
                static_cast<std::byte>('y'),
                static_cast<std::byte>('t'),
                static_cast<std::byte>('e'),
                static_cast<std::byte>('s'),
        };
        bspan sp{value.data(), value.size()};
        auto formatted = "{}"_format(sp);
        CHECK(formatted == "[98, 121, 116, 101, 115]");
    }

}  // namespace un::log::test
