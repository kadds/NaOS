#include "kernel/arch/task.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/trace.hpp"

namespace arch::task
{
u64 stack_size = kernel_stack_page_count * memory::page_size;

void init(void *task, register_info_t *register_info)
{

    void *rsp0 = (char *)current_stack();

    register_info->cr2 = 0;
    register_info->error_code = 0;
    register_info->fs = 0x10;
    register_info->gs = 0x10;
    register_info->rsp = (u64)rsp0;
    register_info->rsp0 = (u64)rsp0;
    register_info->trap_vector = 0;
    tss::set_rsp(0, rsp0);

    thread_info_t *thread_info = new ((void *)register_info->rsp0) thread_info_t();
    thread_info->task = task;
}

ExportC void switch_task(register_info_t *prev, register_info_t *next)
{
    trace::debug("prev task rip: ", (void *)prev->rip, ", next task rip: ", (void *)next->rip, ".");
    tss::set_rsp(0, (void *)next->rsp0);
}
ExportC void task_do_exit(u64 exit_code)
{
    trace::debug("thread exit!");
    while (1)
        ;
}
u64 do_fork(void *task, void *stack_addr, regs_t &regs, register_info_t &register_info, void *function, u64 arg)
{
    util::memset(&regs, 0, sizeof(regs));

    regs.rbx = (u64)function;
    regs.rdx = (u64)arg;

    regs.ds = 0x10;
    regs.es = 0x10;
    regs.cs = 0x08;
    regs.ss = 0x10;
    regs.rflags = (1 << 9);
    regs.rip = (u64)_kernel_thread;

    register_info.rip = regs.rip;
    register_info.rsp = (u64)stack_addr - sizeof(regs);
    register_info.rsp0 = (u64)stack_addr;
    register_info.fs = 0x10;
    register_info.gs = 0x10;

    thread_info_t *thread_info = new (stack_addr) thread_info_t();
    thread_info->task = task;

    return 0;
}

} // namespace arch::task
