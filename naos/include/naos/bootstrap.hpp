#pragma once

#include "naos/abi.h"

namespace naos::bootstrap
{

inline bool valid_message(const na_bootstrap_message_t &message, uint64_t actual_resources)
{
    if (message.struct_size < sizeof(message) || message.flags != 0 ||
        message.version != NA_BOOTSTRAP_MESSAGE_VERSION || message.resource_count < NA_BOOTSTRAP_MIN_RESOURCE_COUNT ||
        message.resource_count > NA_CHANNEL_MAX_RESOURCES || actual_resources != message.resource_count ||
        message.capability_count > NA_BOOTSTRAP_MAX_CAPABILITIES || message.reserved1 != 0 || message.reserved2 != 0 ||
        message.reserved3 != 0)
        return false;

    const uint32_t directories[] = {
        message.root_directory,
        message.current_directory,
        message.service_directory,
    };
    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
    {
        if (directories[i] >= actual_resources)
            return false;
        for (uint32_t j = 0; j < i; j++)
        {
            if (directories[i] == directories[j])
                return false;
        }
    }

    const uint32_t streams[] = {message.stdin_stream, message.stdout_stream, message.stderr_stream};
    for (const auto index : streams)
    {
        if (index >= actual_resources)
            return false;
    }

    for (uint32_t i = 0; i < message.capability_count; i++)
    {
        const auto &capability = message.capabilities[i];
        if (capability.kind == 0 || capability.resource >= actual_resources)
            return false;
        for (const auto index : directories)
        {
            if (capability.resource == index)
                return false;
        }
        for (const auto index : streams)
        {
            if (capability.resource == index)
                return false;
        }
        for (uint32_t j = 0; j < i; j++)
        {
            if (message.capabilities[j].kind == capability.kind ||
                message.capabilities[j].resource == capability.resource)
                return false;
        }
    }
    return true;
}

} // namespace naos::bootstrap
