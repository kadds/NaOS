#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "freelibcxx/formatter.hpp"

TEST_CASE("brace formatter stops character arrays at their terminator")
{
    char value[32] = "hello";
    char buffer[64]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(buffer, sizeof(buffer) - 1), "{}", value);

    REQUIRE(result.error == freelibcxx::format_error::none);
    REQUIRE_FALSE(result.truncated);
    buffer[result.written] = '\0';
    REQUIRE(std::string_view(buffer) == "hello");
}
