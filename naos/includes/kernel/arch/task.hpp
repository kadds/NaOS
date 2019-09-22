#pragma once
#include "common.hpp"

namespace arch::task
{
/* User space memory zone
0x0000,0000,0000,0000 - 0x0000,0000,003F,FFFF: resevered (256MB)
0x0000,0000,0040,0000 - 0x0000,0000,XXXX,XXXX: text
0x0000,0000,XXXX,XXXX - 0x0000,000X,XXXX,XXXX: head
0x0000,000X,XXXX,XXXX - 0x0000,7FFF,EFFF,FFFF: mmap zone
0x0000,7FFF,F000,0000 - 0x0000,8000,0000,0000: stack (256MB)
*/

const u64 user_stack_start_address = 0x0000800000000000;
// stack size: 4MB
const u64 user_stack_end_address = user_stack_start_address - 0x400000;
// maximum stack size: 256MB
const u64 user_stack_maximum_end_address = user_stack_start_address - 0x10000000;

const u64 user_mmap_start_address = user_stack_maximum_end_address;
const u64 user_code_start_address = 0x400000;

// kernel stack size in task
extern u64 kernel_stack_size;

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
    __asm__ __volatile__("andq %%rsp,%0	\n\t" : "=r"(stack) : "0"(~(kernel_stack_size - 1)));
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
    thread_info_t *current = (thread_info_t *)((u64)stack & (~(kernel_stack_size - 1)));
    if (likely(current->is_valid()))
        return current->task;
    return nullptr;
}
void init(void *task, register_info_t *first_task_reg_info);

u64 do_fork(void *task, void *stack_addr, register_info_t &register_info, void *function, u64 arg);
u64 do_exec(void *func, void *stack_addr, void *kernel_stack);
} // namespace arch::task
