#include "naos/abi.h"
#include "naos/bootstrap.hpp"

#include <cassert>

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
    assert(naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    auto extended = message;
    extended.resource_count++;
    assert(naos::bootstrap::valid_message(extended, NA_BOOTSTRAP_RESOURCE_COUNT + 1));
}

void test_rejects_duplicate_or_out_of_range_resources()
{
    auto message = valid_message();
    message.stderr_stream = message.stdin_stream;
    assert(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    message = valid_message();
    message.stderr_stream = NA_BOOTSTRAP_RESOURCE_COUNT;
    assert(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));
}

void test_rejects_unknown_version_and_flags()
{
    auto message = valid_message();
    message.version++;
    assert(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));

    message = valid_message();
    message.flags = 1;
    assert(!naos::bootstrap::valid_message(message, NA_BOOTSTRAP_RESOURCE_COUNT));
}
} // namespace

int main()
{
    test_valid_message();
    test_rejects_duplicate_or_out_of_range_resources();
    test_rejects_unknown_version_and_flags();
    return 0;
}
