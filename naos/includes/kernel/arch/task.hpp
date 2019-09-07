#pragma once
#include "common.hpp"

namespace arch::task
{
// kernel stack size in task
extern u64 stack_size;

// register info in task
struct register_info_t
{
    u64 rsp0; // base rsp
    u64 rip;
    u64 rsp;
    u64 fs;
    u64 gs;
    u64 cr2;
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
    bool is_valid() { return magic_wall == 0xFFCCFFCCCCFFCCFF && magic_end_wall == 0x0AEEAAEEEEAAEE0A; }
    thread_info_t()
        : magic_wall(0xFFCCFFCCCCFFCCFF)
        , magic_end_wall(0x0AEEAAEEEEAAEE0A)
    {
    }
};

// 32 kb stack size in X86_64 arch
const int kernel_stack_page_count = 8;

inline void *current_stack()
{
    void *stack = nullptr;
    __asm__ __volatile__("andq %%rsp,%0	\n\t" : "=r"(stack) : "0"(~(stack_size - 1)));
    return stack;
}

inline void *current_task()
{
    thread_info_t *current = (thread_info_t *)current_stack();
    return current->task;
}

void init(void *task, register_info_t *first_task_reg_info);

u64 do_fork(void *task, void *stack_addr, regs_t &regs, register_info_t &register_info, void *function, u64 arg);
} // namespace arch::task
