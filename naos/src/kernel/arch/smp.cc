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
volatile u32 time_tick = 0;
void smp_watch(u64 pass, u64 data) { time_tick = 1; }
void init()
{
    byte *code_start = (byte *)_ap_code_start, *code_end = (byte *)_ap_code_end;
    util::memcopy(memory::kernel_phyaddr_to_virtaddr((void *)base_ap_phy_addr),
                  memory::kernel_phyaddr_to_virtaddr((void *)code_start), code_end - code_start);
    _mfence();
    u32 cpu_stack =
        (u64)memory::kernel_virtaddr_to_phyaddr(memory::KernelBuddyAllocatorV->allocate(memory::page_size * 4, 0));
    u32 *ap_addr = (u32 *)_ap_stack;
    *memory::kernel_phyaddr_to_virtaddr(ap_addr) = cpu_stack;
    APIC::local_post_init_IPI();
    __asm__ __volatile__("wbinvd	\n\t"
                         "mfence			\n\t"
                         :
                         :
                         : "memory");
    APIC::local_post_start_up((u64)base_ap_phy_addr);
    volatile u32 last_value = 0;
    trace::debug("Wait for ap startup.");
    while (1)
    {
        time_tick = 0;
        timer::add_watcher(200000, smp_watch, 0);
        while (time_tick == 0)
        {
            __asm__ __volatile("pause ");
        }
        u32 v = *(memory::kernel_phyaddr_to_virtaddr((u32 *)_ap_count));
        if (last_value == v)
        {
            last_value = v;
            break;
        }
        last_value = v;
    }
    trace::debug("AP count: ", (u32)last_value);

    memory::KernelBuddyAllocatorV->deallocate((void *)memory::kernel_phyaddr_to_virtaddr((u64)cpu_stack));
    // send INIT IPI
}
} // namespace arch::SMP
