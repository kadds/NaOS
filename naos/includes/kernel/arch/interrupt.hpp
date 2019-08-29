#pragma once
#include "common.hpp"
#include "idt.hpp"
namespace interrupt
{

typedef void request_func(const idt::regs_t *regs, u64 user_data);
struct request_func_data
{
    request_func *func;
    u64 user_data;
};

void init();
void insert_request_func(u32 vector, request_func *func, u64 user_data);
void remove_request_func(u32 vector, request_func *func);

} // namespace interrupt
