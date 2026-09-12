#pragma once

#include "naos/abi.h"

namespace naos::bootstrap
{

inline bool valid_message(const na_bootstrap_message_t &message, uint64_t actual_resources)
{
    const bool early_service = message.flags == NA_BOOTSTRAP_FLAG_EARLY_SERVICE;
    const auto minimum_resources = early_service ? NA_BOOTSTRAP_EARLY_MIN_RESOURCE_COUNT : NA_BOOTSTRAP_MIN_RESOURCE_COUNT;
    if (message.struct_size < sizeof(message) || (message.flags != 0 && !early_service) ||
        message.version != NA_BOOTSTRAP_MESSAGE_VERSION || message.resource_count < minimum_resources ||
        message.resource_count > NA_CHANNEL_MAX_RESOURCES || actual_resources != message.resource_count ||
        message.reserved0 != 0 || message.reserved1 != 0)
        return false;

    if (early_service && (message.root_directory != NA_BOOTSTRAP_RESOURCE_NONE ||
                          message.current_directory != NA_BOOTSTRAP_RESOURCE_NONE))
        return false;

    const uint32_t directories[] = {
        message.root_directory,
        message.current_directory,
        message.service_directory,
    };
    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
    {
        if (early_service && i < 2)
            continue;
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
        if (index == message.service_directory)
            return false;
    }

    return true;
}

} // namespace naos::bootstrap
