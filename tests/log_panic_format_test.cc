#include "catch2_compat.hpp"
#include "kernel/log.hpp"

TEST_CASE("panic records do not use the normal log metadata prefix")
{
    REQUIRE_FALSE(log::record_has_metadata_prefix(log::level::panic));
    REQUIRE(log::record_has_metadata_prefix(log::level::error));
}
