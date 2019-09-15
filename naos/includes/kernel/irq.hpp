#pragma once
#include "arch/idt.hpp"
#include "common.hpp"
namespace irq
{

typedef void (*request_func)(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data);
struct request_func_data
{
    request_func func;
    u64 user_data;
    request_func_data(request_func func, u64 user_data)
        : func(func)
        , user_data(user_data)
    {
    }
};
void init();

void insert_request_func(u32 vector, request_func func, u64 user_data);
void remove_request_func(u32 vector, request_func func);
} // namespace irq