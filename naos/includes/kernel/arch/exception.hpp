#pragma once
#include "common.hpp"
#include "idt.hpp"
namespace arch::exception
{

void init();
void insert_request_func(u32 vector, idt::request_func *func, u64 user_data);
void remove_request_func(u32 vector, idt::request_func *func);
} // namespace arch::exception
