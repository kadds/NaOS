#include "naos/abi.h"
#include "naos/bootstrap.hpp"

#include "catch2_compat.hpp"

namespace
{
na_bootstrap_message_t valid_message()
{
    na_bootstrap_message_t message{};
    message.struct_size = sizeof(message);
    message.version = NA_BOOTSTRAP_MESSAGE_VERSION;
    message.resource_count = NA_BOOTSTRAP_RESOURCE_COUNT;
    message.root_directory = NA_BOOTSTRAP_RESOURCE_ROOT_DIRECTORY;
    message.current_directory = NA_BOOTSTRAP_RESOURCE_CURRENT_DIRECTORY;
    message.service_directory = NA_BOOTSTRAP_RESOURCE_SERVICE_DIRECTORY;
    message.stdin_stream = NA_BOOTSTRAP_RESOURCE_STDIN;
    message.stdout_stream = NA_BOOTSTRAP_RESOURCE_STDOUT;
    message.stderr_stream = NA_BOOTSTRAP_RESOURCE_STDERR;
    return message;
}

void test_valid_message()
{
    const auto message = valid_message();
    REQUIRE(naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    auto extended = message;
    extended.resource_count++;
    REQUIRE(naos::bootstrap::valid_message(extended, NA_BOOTSTRAP_RESOURCE_COUNT + 1));
}

void test_allows_shared_stdio_binding_and_rejects_bad_resources()
{
    auto message = valid_message();
    message.stderr_stream = message.stdin_stream;
    REQUIRE(naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    message.resource_count = NA_BOOTSTRAP_MIN_RESOURCE_COUNT;
    message.stdout_stream = message.stdin_stream;
    REQUIRE(naos::bootstrap::valid_message(message, NA_BOOTSTRAP_MIN_RESOURCE_COUNT));

    message = valid_message();
    message.stderr_stream = NA_BOOTSTRAP_RESOURCE_COUNT;
    REQUIRE(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    message = valid_message();
    message.current_directory = message.root_directory;
    REQUIRE(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));
}

void test_rejects_unknown_version_and_flags()
{
    auto message = valid_message();
    message.version++;
    REQUIRE(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    message = valid_message();
    message.flags = 1;
    REQUIRE(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));
}

void test_early_service_message_allows_service_discovery()
{
    auto message = valid_message();
    message.flags = NA_BOOTSTRAP_FLAG_EARLY_SERVICE;
    message.resource_count = NA_BOOTSTRAP_EARLY_MIN_RESOURCE_COUNT;
    message.root_directory = NA_BOOTSTRAP_RESOURCE_NONE;
    message.current_directory = NA_BOOTSTRAP_RESOURCE_NONE;
    message.service_directory = 0;
    message.stdin_stream = 1;
    message.stdout_stream = 2;
    message.stderr_stream = 3;
    REQUIRE(naos::bootstrap::valid_message(message, NA_BOOTSTRAP_EARLY_MIN_RESOURCE_COUNT));

    message.root_directory = 0;
    REQUIRE(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_EARLY_MIN_RESOURCE_COUNT));
}
} // namespace

TEST_CASE("bootstrap message contract", "[bootstrap]")
{
    test_valid_message();
    test_allows_shared_stdio_binding_and_rejects_bad_resources();
    test_rejects_unknown_version_and_flags();
    test_early_service_message_allows_service_discovery();
}
