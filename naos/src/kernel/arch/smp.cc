#include "kernel/arch/smp.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"

namespace arch::SMP
{
volatile bool tick = false;
std::atomic_int counter;

void timer_tick(u64 pass, u64 user_data) { tick = true; }

void init()
{
    if (!cpu::current().is_bsp())
    {
        counter--;
        while (counter > 0)
        {
            cpu_pause();
        }
        return;
    }
    byte *code_start = (byte *)_ap_code_start, *code_end = (byte *)_ap_code_end;
    util::memcopy(memory::kernel_phyaddr_to_virtaddr((void *)base_ap_phy_addr),
                  memory::kernel_phyaddr_to_virtaddr((void *)code_start), code_end - code_start);
    _mfence();
    u32 cpu_stack =
        (u64)memory::kernel_virtaddr_to_phyaddr(memory::KernelBuddyAllocatorV->allocate(memory::kernel_stack_size, 0));
    u32 *ap_addr = (u32 *)_ap_stack;
    *memory::kernel_phyaddr_to_virtaddr(ap_addr) = cpu_stack;
    trace::debug("Send INIT-IPI");
    APIC::local_post_init_IPI();
    trace::debug("Send StartUP-IPI");
    APIC::local_post_start_up((u64)base_ap_phy_addr);

    trace::debug("Wait for AP startup.");
    u32 count = 0;
    volatile u32 *ap_count = (volatile u32 *)(memory::kernel_phyaddr_to_virtaddr((u32 *)_ap_count));
    while (1)
    {
        timer::add_watcher(100000, timer_tick, 0);
        while (!tick)
        {
            cpu_pause();
        }
        if (*ap_count == count)
        {
            break;
        }
        count = *ap_count;
        tick = false;
    }
    trace::debug("AP count: ", count);
    counter = count + 1;

    volatile u32 *v = (volatile u32 *)(memory::kernel_phyaddr_to_virtaddr((u32 *)_ap_stack));
    volatile u32 *standby = (volatile u32 *)(memory::kernel_phyaddr_to_virtaddr((u32 *)_ap_standby));

    for (u32 i = 0; i < count; i++)
    {
        cpu_stack = (u64)memory::kernel_virtaddr_to_phyaddr(
            memory::KernelBuddyAllocatorV->allocate(memory::kernel_stack_size, 0));
        *v = cpu_stack + memory::kernel_stack_size;
        _mfence();
        *standby = 0;
        trace::debug("Wait AP ", i + 1, ".");
        while (*v != 0)
        {
            cpu_pause();
        }
    }
    counter--;
    while (counter > 0)
    {
        cpu_pause();
    }
}

int count()
{
    volatile u32 *ap_count = (volatile u32 *)(memory::kernel_phyaddr_to_virtaddr((u32 *)_ap_count));
    return *ap_count;
}
} // namespace arch::SMP
