#pragma once
#include "../mm/mm.hpp"
#include "common.hpp"
namespace task
{
struct thread_t;
} // namespace task

namespace arch::task
{

// register info in task
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

// registers in stack
struct regs_t
{
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 ds;
    u64 es;
    u64 rax;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

struct thread_info_t
{
    u64 magic_wall;
    void *task;
    u64 magic_end_wall;
    // check for kernel stack overflow
    bool is_valid() const { return magic_wall == 0xFFCCFFCCCCFFCCFF && magic_end_wall == 0x0AEEAAEEEEAAEE0A; }
    thread_info_t()
        : magic_wall(0xFFCCFFCCCCFFCCFF)
        , magic_end_wall(0x0AEEAAEEEEAAEE0A)
    {
    }
};

inline void *current_stack()
{
    void *stack = nullptr;
    __asm__ __volatile__("andq %%rsp,%0	\n\t" : "=r"(stack) : "0"(~(memory::kernel_stack_size - 1)));
    return stack;
}

inline void *current_task()
{
    thread_info_t *current = (thread_info_t *)current_stack();
    if (likely(current->is_valid()))
        return current->task;
    return nullptr;
}

inline void *get_task(void *stack)
{
    thread_info_t *current = (thread_info_t *)((u64)stack & (~(memory::kernel_stack_size - 1)));
    if (likely(current->is_valid()))
        return current->task;
    return nullptr;
}
void init(::task::thread_t *thd, register_info_t *first_task_reg_info);

u64 create_thread(::task::thread_t *thd, void *function, u64 arg0, u64 arg1, u64 arg2, u64 arg3);
u64 enter_userland(::task::thread_t *thd, void *entry, u64 arg);
} // namespace arch::task
