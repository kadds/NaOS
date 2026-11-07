#pragma once

#include "kernel/kernel.hpp"

namespace arch::multiboot
{

/// Append one Multiboot module's physical extent and optional service token.
/// `command_line_size` is the byte count available in the validated tag.
__attribute__((always_inline)) inline bool append_boot_module(boot_module *modules, u64 capacity, u64 &count, u64 start,
                                                              u64 end, const char *command_line,
                                                              u64 command_line_size) noexcept
{
    if (modules == nullptr || count >= capacity || end < start || command_line == nullptr || command_line_size == 0)
        return false;

    u64 token_length = 0;
    while (token_length < command_line_size && command_line[token_length] != '\0')
        token_length++;
    if (token_length == command_line_size)
        return false;

    auto &module = modules[count];
    for (u64 index = 0; index < sizeof(module.name); index++)
        module.name[index] = '\0';
    const u64 token_capacity = sizeof(module.name) - 1;
    const u64 copied_length = token_length < token_capacity ? token_length : token_capacity;
    for (u64 index = 0; index < copied_length; index++)
        module.name[index] = command_line[index];
    module.start = start;
    module.size = end - start;
    count++;
    return true;
}

} // namespace arch::multiboot
