#include "catch2_compat.hpp"
#include "kernel/log.hpp"
#include "kernel/log_format.hpp"

#include <string>

TEST_CASE("panic records do not use the normal log metadata prefix")
{
    REQUIRE_FALSE(log::record_has_metadata_prefix(log::level::panic));
    REQUIRE(log::record_has_metadata_prefix(log::level::error));
}

TEST_CASE("structured log escaping preserves line breaks")
{
    char buffer[128]{};
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(buffer, sizeof(buffer))};
    constexpr char message[] = "first\n    second\r\t\x1b";

    log::format::append_escaped(output, message, sizeof(message) - 1);

    REQUIRE(std::string(buffer, output.written) == "first\n    second\\r\\t\\x1b");
}
