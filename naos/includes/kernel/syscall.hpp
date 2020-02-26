#pragma once
#include "common.hpp"
namespace syscall
{
ExportC void *system_call_table[];

#define SYSCALL(idx, name) system_call_table[idx] = ((void *)&(name));
#define BEGIN_SYSCALL                                                                                                  \
    static void syscall_init()                                                                                         \
    {
#define END_SYSCALL                                                                                                    \
    }                                                                                                                  \
    ;                                                                                                                  \
    static Section(".init_array") __attribute__((__used__)) void *syscall_init_ptr = (void *)&syscall_init;
} // namespace syscall
#define OK 0
#define EOF -1
#define ETIMEOUT -2
#define EINTR -3
#define EPARAM -4
#define EBUFFER -5
#define ESIZE -6
#define EPERMISSION -7
#define ERESOURCE_NOT_NULL -8
#define ENOEXIST -9
#define EINNER -10
#define EFAILED -11
#define ECONTI -12ul
