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
std::atomic_int last_cpuid = 0;

void *get_rsp()
{
    void *stack;
    __asm__ __volatile__("movq %%rsp,%0	\n\t" : "=r"(stack) : :);
    return stack;
}

void cpu_t::set_context(void *stack)
{
    kernel_rsp = stack;
    tss::set_rsp(cpu::current().get_id(), 0, (void *)kernel_rsp);
}

bool cpu_t::is_in_exception_context()
{
    byte *krsp = (byte *)get_exception_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::exception_stack_size - 1)) + memory::exception_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_kernel_context()
{
    byte *krsp = (byte *)get_kernel_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::kernel_stack_size - 1)) + memory::kernel_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_interrupt_context()
{
    byte *krsp = (byte *)get_interrupt_rsp();
    byte *rrsp = (byte *)(((u64)get_rsp() & ~(memory::interrupt_stack_size - 1)) + memory::interrupt_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_exception_context(void *rsp)
{
    byte *krsp = (byte *)rsp;
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::exception_stack_size - 1)) + memory::exception_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_interrupt_context(void *rsp)
{
    byte *krsp = (byte *)get_interrupt_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::interrupt_stack_size - 1)) + memory::interrupt_stack_size);

    return rrsp == krsp;
}

bool cpu_t::is_in_kernel_context(void *rsp)
{
    byte *krsp = (byte *)get_kernel_rsp();
    byte *rrsp = (byte *)(((u64)rsp & ~(memory::kernel_stack_size - 1)) + memory::kernel_stack_size);

    return rrsp == krsp;
}

cpuid_t init()
{
    u64 cpuid = last_cpuid++;
    auto &cur_data = per_cpu_data[cpuid];
    cur_data.id = cpuid;
    /// gs kernel base
    _wrmsr(0xC0000102, (u64)&per_cpu_data[cpuid]);
    kassert(_rdmsr(0xC0000102) == ((u64)&per_cpu_data[cpuid]), "Unable to write kernel gs_base");
    /// gs user base
    _wrmsr(0xC0000101, 0);
    /// fs base
    _wrmsr(0xC0000100, 0);
    __asm__("swapgs \n\t" ::: "memory");
    kassert(_rdmsr(0xC0000101) == ((u64)&per_cpu_data[cpuid]), "Unable to swap kernel gs register");

    return cpuid;
}

bool has_init() { return last_cpuid >= 1; }

void init_data(cpuid_t cpuid)
{
    _wrmsr(0xC0000101, (u64)&per_cpu_data[cpuid]);

    auto &data = cpu::current();
    data.interrupt_rsp =
        (byte *)memory::KernelBuddyAllocatorV->allocate(memory::interrupt_stack_size, 0) + memory::interrupt_stack_size;
    data.exception_rsp =
        (byte *)memory::KernelBuddyAllocatorV->allocate(memory::exception_stack_size, 0) + memory::exception_stack_size;
    data.exception_nmi_rsp = (byte *)memory::KernelBuddyAllocatorV->allocate(memory::exception_nmi_stack_size, 0) +
                             memory::exception_nmi_stack_size;

    u64 krsp = ~(memory::kernel_stack_size - 1);
    __asm__ __volatile__("andq %%rsp, %0\n\t" : "+g"(krsp)::"memory");
    auto kernel_rsp = (void *)(krsp + memory::kernel_stack_size);
    data.kernel_rsp = kernel_rsp;
    tss::set_rsp(cpuid, 0, (void *)kernel_rsp);

    tss::set_ist(cpuid, 1, data.interrupt_rsp);
    tss::set_ist(cpuid, 3, data.exception_rsp);
    tss::set_ist(cpuid, 4, data.exception_nmi_rsp);
}

bool cpu_t::is_bsp() { return id == 0; }

cpu_t &get(cpuid_t cpuid) { return per_cpu_data[cpuid]; }

cpu_t &current()
{
    u64 cpuid;
#ifdef _DEBUG
    kassert(_rdmsr(0xC0000101) != 0, "Unreadable gs base");
    kassert(_rdmsr(0xC0000102) == 0, "Unreadable kernel gs base");
#endif
    __asm__("movq %%gs:0x0, %0\n\t" : "=r"(cpuid) : :);
    return per_cpu_data[cpuid];
}
void *current_user_data()
{
    u64 u;
    __asm__("movq %%gs:0x10, %0\n\t" : "=r"(u) : :);
    return (void *)u;
}

u64 count() { return last_cpuid; }

cpuid_t id() { return current().get_id(); }

} // namespace arch::cpu