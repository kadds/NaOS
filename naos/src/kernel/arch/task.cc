#include "kernel/arch/task.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/trace.hpp"

namespace arch::task
{

void init(::task::thread_t *thd, register_info_t *register_info)
{
    void *stack_low = (char *)current_stack();

    register_info->cr2 = 0;
    register_info->error_code = 0;
    register_info->fs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info->gs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info->rsp = (void *)((u64)stack_low + memory::kernel_stack_size);
    register_info->trap_vector = 0;

    thread_info_t *thread_info = new (stack_low) thread_info_t();
    thread_info->task = thd;
    thd->kernel_stack_top = (void *)register_info->rsp;

    _wrmsr(0xC0000080, _rdmsr(0xC0000080) | 1);
    constexpr u64 selector = ((u64)(gdt::gen_selector(arch::gdt::selector_type::kernel_code, 0)) << 32) |
                             ((u64)(gdt::gen_selector(arch::gdt::selector_type::user_code, 3) - 16) << 48);
    _wrmsr(0xC0000081, selector);
    _wrmsr(0xC0000082, (u64)&_sys_call);
    _wrmsr(0xC0000084, 0x200); // rFLAGS clean bits

    cpu::current().set_context(thd);
}

ExportC void switch_task(register_info_t *prev, register_info_t *next, ::task::thread_t *thd)
{
    cpu::current().set_context(thd);
}

ExportC void do_exit(u64 exit_code)
{
    trace::debug("thread exit!");
    while (1)
        ;
}

u64 create_thread(::task::thread_t *thd, void *function, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    regs_t regs;
    util::memset(&regs, 0, sizeof(regs));

    regs.rbx = (u64)function;
    regs.rdi = arg0;
    regs.rsi = arg1;
    regs.rdx = arg2;
    regs.rcx = arg3;

    regs.ds = gdt::gen_selector(gdt::selector_type::kernel_data, 0);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::kernel_code, 0);
    regs.ss = regs.ds;
    regs.rflags = (1 << 9); // enable IF
    regs.rip = (u64)_kernel_thread;
    auto &register_info = *thd->register_info;
    register_info.rip = (void *)regs.rip;
    register_info.rsp = (void *)((u64)thd->kernel_stack_top - sizeof(regs));
    register_info.fs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info.gs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);

    thread_info_t *thread_info = new ((void *)((u64)thd->kernel_stack_top - memory::kernel_stack_size)) thread_info_t();
    thread_info->task = thd;

    util::memcopy((void *)((u64)thd->kernel_stack_top - sizeof(regs)), &regs, sizeof(regs_t));

    return 0;
}

u64 enter_userland(::task::thread_t *thd, void *entry, u64 arg)
{
    regs_t regs;
    util::memzero(&regs, sizeof(regs));
    thd->register_info->rip = (void *)&_sys_ret;
    regs.rcx = (u64)entry;
    regs.r10 = (u64)thd->user_stack_top;
    regs.r11 = (1 << 9); // rFLAGS: enable IF
    regs.ds = gdt::gen_selector(gdt::selector_type::user_data, 3);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::user_code, 3);
    regs.rdi = arg;
    uctx::UnInterruptableContext icu;
    cpu::current().set_context(thd);
    _call_sys_ret(&regs);
    return 1;
}

} // namespace arch::task
