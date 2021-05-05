#include "kernel/arch/smp.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"
/**
 * ap startup memory:
 * 0x70000 ap startup code
 * 0x1000 ap startup page tables
 * kernel bsp stack -> 0x90000
 * ap stack -> 0x80000
 */

namespace arch::SMP
{
volatile bool tick = false;
std::atomic_int counter;
volatile void * ap_stack;
volatile void * ap_stack_phy;

void timer_tick(u64 pass, u64 user_data) { tick = true; }

void init()
{
    if (!cpu::current().is_bsp())
    {
        counter--;
        trace::debug("AP ", cpu::current().get_id(), " is arrived entrypoint");
        volatile u64 *stack = (volatile u64 *)memory::pa2va<u64 *>(phy_addr_t::from((u64)_ap_stack));
        *stack = 0;
        while (counter > 0)
        {
            cpu_pause();
        }
        return;
    }
    phy_addr_t code_start = phy_addr_t::from((byte *)_ap_code_start), code_end = phy_addr_t::from((byte *)_ap_code_end);
    util::memcopy(memory::pa2vax((byte *)base_ap_phy_addr), memory::pa2va(code_start), code_end - code_start);

    volatile u64 *stack = (volatile u64 *)memory::pa2va<u64 *>(phy_addr_t::from((u64)_ap_stack));
    volatile u32 *startup_flag = (volatile u32 *)memory::pa2va<u32 *>(phy_addr_t::from((u64)_ap_startup_spin_flag));
    // volatile u32 *ap_count = (volatile u32 *)memory::pa2va<u32 *>(phy_addr_t::from((u64)_ap_count));
    *startup_flag = 1;
    _mfence();

    trace::debug("Sending INIT-IPI");
    APIC::local_post_init_IPI();

    trace::debug("Sending StartUP-IPI");
    APIC::local_post_start_up((u64)base_ap_phy_addr);
    // APIC::local_post_start_up((u64)base_ap_phy_addr);
    u32 count = cpu_info::get_cpu_mesh_info().logic_num;
    u32 idx = 1;
    if (count > cpu::max_cpu_support)
    {
        trace::warning("max cpu support ", cpu::max_cpu_support, " but current machine cpus ", count);
        count = cpu::max_cpu_support;
    }
    counter = count;
    trace::debug("Waiting for AP startup");

    while (idx < count)
    {
        void *virtual_ap_stack = cpu::get_kernel_stack_bottom(idx);
        ap_stack = virtual_ap_stack;
        ap_stack_phy = cpu::get_kernel_stack_bottom_phy(idx)();
        u64 cpu_stack = (u64)virtual_ap_stack;
        _mfence();
        *stack = cpu_stack + memory::kernel_stack_size;
        _mfence();
        *startup_flag = 0;
        _mfence();

        while (*stack != 0)
        {
            cpu_pause();
        }
        idx++;
    }

    trace::debug("Bsp is arrived entrypoint");
    trace::debug("Waiting for all processtor to arrive entrypoint");
    counter--;
    while (counter > 0)
    {
        cpu_pause();
    }
}

} // namespace arch::SMP
