#include "kernel/arch/task.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/trace.hpp"
namespace arch::task
{
u64 kernel_stack_size = kernel_stack_page_count * memory::page_size;

void init(void *task, register_info_t *register_info)
{

    void *stack_low = (char *)current_stack();

    register_info->cr2 = 0;
    register_info->error_code = 0;
    register_info->fs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info->gs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info->rsp = (u64)stack_low + kernel_stack_size;
    register_info->rsp0 = register_info->rsp;
    register_info->trap_vector = 0;
    tss::set_rsp(0, (void *)register_info->rsp0);

    thread_info_t *thread_info = new (stack_low) thread_info_t();
    thread_info->task = task;

    _wrmsr(0xC0000080, _rdmsr(0xC0000080) | 1);
    constexpr u64 selector = ((u64)(gdt::gen_selector(arch::gdt::selector_type::kernel_code, 0)) << 32) |
                             ((u64)(gdt::gen_selector(arch::gdt::selector_type::user_code, 3) - 16) << 48);
    _wrmsr(0xC0000081, selector);
    _wrmsr(0xC0000082, (u64)&_sys_call);
    _wrmsr(0xC0000084, 0); // rFLAGS clean bits
}

ExportC void switch_task(register_info_t *prev, register_info_t *next)
{
    trace::debug("prev task rip: ", (void *)prev->rip, ", next task rip: ", (void *)next->rip, ".");
    trace::debug("gs kernel base: ", (void *)_rdmsr(0xC0000102));
    trace::debug("gs base: ", (void *)_rdmsr(0xC0000101));

    tss::set_rsp(0, (void *)next->rsp0);
    tss::set_ist(1, (void *)next->rsp0);
    cpu::set_pre_cpu_rsp(0, (void *)next->rsp0);
}

ExportC void task_do_exit(u64 exit_code)
{
    trace::debug("thread exit!");
    while (1)
        ;
}

u64 do_fork(void *task, void *stack_low, regs_t &regs, register_info_t &register_info, void *function, u64 arg)
{
    util::memset(&regs, 0, sizeof(regs));

    regs.rbx = (u64)function;
    regs.rdx = (u64)arg;

    regs.ds = gdt::gen_selector(gdt::selector_type::kernel_data, 0);
    regs.es = regs.ds;
    regs.cs = gdt::gen_selector(gdt::selector_type::kernel_code, 0);
    regs.ss = regs.ds;
    regs.rflags = (1 << 9); // enable IF
    regs.rip = (u64)_kernel_thread;

    register_info.rip = regs.rip;
    register_info.rsp = (u64)stack_low + kernel_stack_size - sizeof(regs);
    register_info.rsp0 = (u64)stack_low + kernel_stack_size;
    register_info.fs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);
    register_info.gs = gdt::gen_selector(arch::gdt::selector_type::kernel_data, 0);

    thread_info_t *thread_info = new (stack_low) thread_info_t();
    thread_info->task = task;

    return 0;
}

u64 do_exec(void *func, void *stack_addr, void *kernel_stack)
{
    regs_t regs;
    util::memzero(&regs, sizeof(regs));
    regs.r11 = 0;
    regs.rcx = (u64)func;
    regs.r10 = (u64)stack_addr;
    regs.r11 = (1 << 9); // rFLAGS: enable IF
    regs.ds = gdt::gen_selector(gdt::selector_type::user_data, 3);
    regs.es = regs.ds;

    tss::set_rsp(0, kernel_stack);
    tss::set_ist(1, kernel_stack);
    cpu::set_pre_cpu_rsp(0, kernel_stack);

    _call_sys_ret(&regs);
    return 1;
}

} // namespace arch::task
