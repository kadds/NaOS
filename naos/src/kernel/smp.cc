#include "kernel/smp.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/lock.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace SMP
{

irq::request_result flush_tlb_irq(const void *regs, u64 data, u64 user_data)
{
    arch::paging::reload();
    return irq::request_result::ok;
}

irq::request_result ipi_call(const void *regs, u64 data, u64 user_data)
{
    auto &cpu = cpu::current();
    cpu.call_cpu();
    cpu.cpu_call_lock().unlock();

    return irq::request_result::ok;
}

void init()
{
    arch::SMP::init();
    if (cpu::current().is_bsp())
    {
        irq::insert_request_func(irq::hard_vector::IPI_tlb, flush_tlb_irq, 0);
        irq::insert_request_func(irq::hard_vector::IPI_call, ipi_call, 0);
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
