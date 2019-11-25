#pragma once
#include "arch/idt.hpp"
#include "common.hpp"
#include "types.hpp"
namespace irq
{
namespace hard_vector
{
enum hard_vector
{
    pit_timer = 32,
    HEPT = 56,
    local_apic_timer = 128,
    IPI = 255,
};
} // namespace hard_vector

namespace soft_vector
{
enum soft_vector
{
    high_task = 0,
    timer = 1,
    net_send = 2,
    net_rec = 3,
    block = 4,
    task = 5,
    sched = 6,
    COUNT,
};
} // namespace soft_vector

struct request_func_data
{
    union
    {
        request_func hard_func;
        soft_request_func soft_func;
        void *func;
    };
    u64 user_data;
    request_func_data(void *func, u64 user_data)
        : func(func)
        , user_data(user_data)
    {
    }
};

void wakeup_soft_irq_daemon();

void raise_soft_irq(u64 soft_irq_number);

void init();

void insert_request_func(u32 vector, request_func func, u64 user_data);
void remove_request_func(u32 vector, request_func func);

void insert_soft_request_func(u32 vector, soft_request_func func, u64 user_data);
void remove_soft_request_func(u32 vector, soft_request_func func);

} // namespace irq