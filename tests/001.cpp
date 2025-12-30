#include "utils.hpp"

namespace un::log::test {
    TEST_CASE("001 - global settings", "[001]") {
        unlog::info("test case beginning");
        REQUIRE(unlog::get_default_level() == LogLevel::info);

        unlog::info("test underway...");
        unlog::debug("shhh");  // will not print

        set_default_level(LogLevel::debug);
        REQUIRE(unlog::get_default_level() == LogLevel::debug);
        unlog::debug("almost done");

        set_default_level();
        REQUIRE(unlog::get_default_level() == LogLevel::info);
        unlog::info("test case {} complete!", 1);
    }

}  // namespace un::log::test
