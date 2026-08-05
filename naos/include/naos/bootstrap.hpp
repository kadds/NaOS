#pragma once

#include "naos/abi.h"

namespace naos::bootstrap
{

inline bool valid_message(const na_bootstrap_message_t &message, uint64_t actual_resources)
{
    if (message.struct_size < sizeof(message) || message.flags != 0 ||
        message.version != NA_BOOTSTRAP_MESSAGE_VERSION || message.resource_count < NA_BOOTSTRAP_RESOURCE_COUNT ||
        message.resource_count > NA_CHANNEL_MAX_RESOURCES || actual_resources != message.resource_count ||
        message.reserved0 != 0 || message.reserved1 != 0 || message.reserved2 != 0 || message.reserved3 != 0)
        return false;

    const uint32_t indices[] = {
        message.root_directory, message.current_directory, message.service_directory,
        message.stdin_stream,   message.stdout_stream,     message.stderr_stream,
    };
    for (uint32_t i = 0; i < NA_BOOTSTRAP_RESOURCE_COUNT; i++)
    {
        if (indices[i] >= actual_resources)
            return false;
        for (uint32_t j = 0; j < i; j++)
        {
            if (indices[i] == indices[j])
                return false;
        }
    }
    return true;
}

} // namespace naos::bootstrap
