#pragma once
#include "common.hpp"
namespace naos::syscall
{
ExportC void *system_call_table[];

#define SYSCALL(idx, name) ::naos::syscall::system_call_table[idx] = ((void *)&(name));
#define BEGIN_SYSCALL                                                                                                  \
    static void syscall_init()                                                                                         \
    {
#define END_SYSCALL                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    static Section(".init_array") __attribute__((__used__)) void *syscall_init_ptr = (void *)&syscall_init;
} // namespace naos::syscall
