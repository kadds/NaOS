#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/task.hpp"
#include "kernel/arch/tss.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace arch::cpu
{
cpu_t per_cpu_data[max_cpu_support];

::lock::spinlock_t lock;
cpuid_t last_cpuid = 0;

void *get_rsp()
{
    void *stack;
    __asm__ __volatile__("movq %%rsp,%0	\n\t" : "=r"(stack) :);
    return stack;
}

void cpu_t::set_context(::task::thread_t *task)
{
    this->task = task;
    if (task != nullptr)
        this->kernel_rsp = task->kernel_stack_top;
    else
        this->kernel_rsp = nullptr;

    tss::set_rsp(0, (void *)kernel_rsp);

    tss::set_ist(1, interrupt_rsp);
    tss::set_ist(2, soft_irq_rsp);
    tss::set_ist(3, exception_rsp);
    tss::set_ist(4, exception_ext_rsp);

    auto thread_info = (arch::task::thread_info_t *)((u64)interrupt_rsp - memory::interrupt_stack_size);
    thread_info->task = task;

    thread_info = (arch::task::thread_info_t *)((u64)exception_rsp - memory::exception_stack_size);
    thread_info->task = task;

    thread_info = (arch::task::thread_info_t *)((u64)exception_ext_rsp - memory::exception_ext_stack_size);
    thread_info->task = task;
}

bool cpu_t::is_in_hard_irq_context()
{
    byte *krsp = (byte *)get_interrupt_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::interrupt_stack_size - 1)) + memory::interrupt_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_exception_context()
{
    byte *krsp = (byte *)get_exception_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::exception_stack_size - 1)) + memory::exception_stack_size);

    byte *krsp2 = (byte *)get_exception_ext_rsp();
    byte *rrsp2 =
        (byte *)(((u64)get_rsp() & ~(memory::exception_ext_stack_size - 1)) + memory::exception_ext_stack_size);

    return rrsp == krsp || krsp2 == rrsp2;
}

bool cpu_t::is_in_soft_irq_context()
{
    byte *krsp = (byte *)get_soft_irq_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::soft_irq_stack_size - 1)) + memory::soft_irq_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_irq_context() { return is_in_soft_irq_context() || is_in_hard_irq_context(); }

bool cpu_t::is_in_kernel_context()
{
    byte *krsp = (byte *)get_kernel_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::kernel_stack_size - 1)) + memory::kernel_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_hard_irq_context(void *rsp)
{
    byte *krsp = (byte *)get_interrupt_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::interrupt_stack_size - 1)) + memory::interrupt_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_exception_context(void *rsp)
{
    byte *krsp = (byte *)get_exception_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::exception_stack_size - 1)) + memory::exception_stack_size);

    byte *krsp2 = (byte *)get_exception_ext_rsp();
    byte *rrsp2 = (byte *)(((u64)rsp & ~(memory::exception_ext_stack_size - 1)) + memory::exception_ext_stack_size);

    return rrsp == krsp || krsp2 == rrsp2;
}

bool cpu_t::is_in_soft_irq_context(void *rsp)
{
    byte *krsp = (byte *)get_soft_irq_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::soft_irq_stack_size - 1)) + memory::soft_irq_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_irq_context(void *rsp) { return is_in_soft_irq_context(rsp) || is_in_hard_irq_context(rsp); }

bool cpu_t::is_in_kernel_context(void *rsp)
{
    byte *krsp = (byte *)get_kernel_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::kernel_stack_size - 1)) + memory::kernel_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_user_context(void *rsp)
{
    byte *ursp = (byte *)get_task()->user_stack_top;
    byte *ubrsp = (byte *)get_task()->user_stack_bottom;
    byte *rrsp = (byte *)(u64)rsp;
    return ursp >= rrsp && ubrsp <= rrsp;
}

cpuid_t init()
{
    uctx::SpinLockContext ctx(lock);
    u64 cpuid = last_cpuid;
    auto &cur_data = per_cpu_data[cpuid];
    cur_data.id = cpuid;
    last_cpuid++;
    /// gs kernel base
    _wrmsr(0xC0000102, (u64)&per_cpu_data[cpuid]);
    kassert(_rdmsr(0xC0000102) == ((u64)&per_cpu_data[cpuid]), "Unable write kernel gs_base");
    /// gs user base
    _wrmsr(0xC0000101, 0);
    /// fs base
    _wrmsr(0xC0000100, 0);
    __asm__("swapgs \n\t" ::: "memory");
    kassert(_rdmsr(0xC0000101) == ((u64)&per_cpu_data[cpuid]), "Unable swap kernel gs");

    return cpuid;
}

void init_data(cpuid_t cpuid)
{
    _wrmsr(0xC0000101, (u64)&per_cpu_data[cpuid]);

    uctx::SpinLockContext ctx(lock);
    auto &data = cpu::current();
    data.interrupt_rsp =
        (byte *)memory::KernelBuddyAllocatorV->allocate(memory::interrupt_stack_size, 0) + memory::interrupt_stack_size;
    data.exception_rsp =
        (byte *)memory::KernelBuddyAllocatorV->allocate(memory::exception_stack_size, 0) + memory::exception_stack_size;
    data.exception_ext_rsp = (byte *)memory::KernelBuddyAllocatorV->allocate(memory::exception_ext_stack_size, 0) +
                             memory::exception_ext_stack_size;
    data.soft_irq_rsp =
        (byte *)memory::KernelBuddyAllocatorV->allocate(memory::soft_irq_stack_size, 0) + memory::soft_irq_stack_size;

    arch::task::thread_info_t *thread_info =
        new ((void *)((u64)data.interrupt_rsp - memory::interrupt_stack_size)) arch::task::thread_info_t();
    thread_info->task = nullptr;

    thread_info = new ((void *)((u64)data.exception_rsp - memory::exception_stack_size)) arch::task::thread_info_t();
    thread_info->task = nullptr;

    thread_info =
        new ((void *)((u64)data.exception_ext_rsp - memory::exception_ext_stack_size)) arch::task::thread_info_t();
    thread_info->task = nullptr;

    data.set_context(nullptr);
    u64 krsp = ~(memory::kernel_stack_size - 1);
    __asm__("andq %%rsp, %0\n\t" : "=r"(krsp)::"memory");
    data.kernel_rsp = (void *)(krsp + memory::kernel_stack_size);

#ifdef _DEBUG
    trace::debug("[cpu", cpuid, "] ersp:", (void *)data.exception_rsp, " irsp:", (void *)data.interrupt_rsp);
#endif
}

/// TODO: identify bsp ap
bool cpu_t::is_bsp() { return false; }

cpu_t &get(cpuid_t cpuid) { return per_cpu_data[cpuid]; }

cpu_t &current()
{
    u64 cpuid;
#ifdef _DEBUG
    kassert(_rdmsr(0xC0000101) != 0, "Unreadable gs base");
    kassert(_rdmsr(0xC0000102) == 0, "Unreadable kernel gs base");
#endif
    __asm__("movq %%gs:0x0, %0\n\t" : "=r"(cpuid)::"memory");
    return per_cpu_data[cpuid];
}

cpuid_t id() { return current().get_id(); }

} // namespace arch::cpu