#pragma once
#include "../mm/mm.hpp"
#include "common.hpp"
#include "regs.hpp"
namespace task
{
struct thread_t;
} // namespace task

namespace arch::task
{

/// register info in task
struct register_info_t
{
    void *rip;
    void *rsp;
    u64 fs;
    u64 gs;
    void *cr2;
    u64 trap_vector;
    u64 error_code;
};

void init(::task::thread_t *thd, register_info_t *first_task_reg_info);

u64 create_thread(::task::thread_t *thd, void *function, u64 arg0, u64 arg1, u64 arg2, u64 arg3);
u64 enter_userland(::task::thread_t *thd, void *entry, u64 arg);

void *copy_to_return_signal(void *stack, void *ptr, u64 size);

struct userland_code_context
{
    u64 *rsp;
    u64 data_stack_rsp;
    u64 *param[4];
    regs_t regs;

    u64 user_data;
};

bool make_signal_context(void *stack, void *func, userland_code_context *context);

u64 copy_signal_param(userland_code_context *context, const void *ptr, u64 size);
void set_signal_param(userland_code_context *context, int index, u64 val);

void set_signal_context(userland_code_context *context);

void return_from_signal_context(userland_code_context *context, u64 code);

} // namespace arch::task
