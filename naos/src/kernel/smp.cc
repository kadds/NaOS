#include "kernel/smp.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"

namespace SMP
{

irq::request_result flush_tlb_irq(const irq::interrupt_info *inter, u64 data, u64 user_data)
{
    arch::paging::page_table_t::reload();
    return irq::request_result::ok;
}

irq::request_result ipi_call(const irq::interrupt_info *inter, u64 data, u64 user_data)
{
    auto &cpu = cpu::current();
    cpu.call_cpu();
    cpu.cpu_call_lock().unlock();

    return irq::request_result::ok;
}

void init()
{
    if (cpu::current().is_bsp())
    {
        trace::debug("SMP init");
    }
    arch::SMP::init();
    if (cpu::current().is_bsp())
    {
        irq::register_request_func(irq::hard_vector::IPI_tlb, flush_tlb_irq, 0);
        irq::register_request_func(irq::hard_vector::IPI_call, ipi_call, 0);
    }
}

void flush_all_tlb() { arch::APIC::local_post_IPI_all_notself(irq::hard_vector::IPI_tlb); }

void reschedule_cpu(u32 cpuid)
{
    arch::APIC::local_post_IPI_mask(irq::hard_vector::IPI_reschedule, arch::cpu::get(cpuid).get_apic_id());
}

void call_cpu(u32 cpuid, cpu::call_cpu_func_t call_func, u64 user_data)
{
    auto &cpu = cpu::get(cpuid);
    cpu.cpu_call_lock().lock();
    cpu.set_call_cpu_func(call_func, user_data);
    arch::APIC::local_post_IPI_mask(irq::hard_vector::IPI_call, arch::cpu::get(cpuid).get_apic_id());
}

} // namespace SMP
