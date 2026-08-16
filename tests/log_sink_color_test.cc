#include "catch2_compat.hpp"
#include "kernel/log.hpp"

TEST_CASE("console and dmesg enable structured field colors by default")
{
    const log::configuration configuration{};
    REQUIRE(configuration.console.color);
    REQUIRE(configuration.dmesg.color);
    REQUIRE_FALSE(configuration.serial.color);
}
