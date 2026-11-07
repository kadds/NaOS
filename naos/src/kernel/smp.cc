#include "kernel/smp.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/irq/ipi_drain.hpp"
#include "kernel/lock.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>

KLOG_MODULE(kernel);
namespace SMP
{

irq::registration *tlb_registration;
irq::registration *call_registration;
irq::registration *call_soft_registration;

namespace
{
constexpr u32 max_call_operations_per_softirq_batch = 16;
constexpr u32 max_msi_drain_polls = 1'000'000;
constexpr u32 cache_line_size = 64;
std::atomic<u64> requested_tlb_flush_generation{0};
std::atomic<u64> acknowledged_tlb_flush_generation[arch::cpu::max_cpu_support]{};

/// Keep each target CPU's drain state on its own cache line. Different route
/// teardowns may drain different CPUs concurrently, so packed per-CPU arrays
/// make unrelated requests invalidate one another's cache lines.
struct alignas(cache_line_size) msi_drain_slot
{
    std::atomic<u64> requested_generation{0};
    std::atomic<u64> acknowledged_generation{0};
    std::atomic<u8> requested_vector{0};
    std::atomic<bool> quarantined{false};
    std::atomic_flag operation_lock = ATOMIC_FLAG_INIT;
};

static_assert(alignof(msi_drain_slot) == cache_line_size);
static_assert(sizeof(msi_drain_slot) % cache_line_size == 0);

msi_drain_slot msi_drain_slots[arch::cpu::max_cpu_support]{};

void drain_call_cpu_queue(u64) noexcept
{
    auto &cpu = cpu::current();
    cpu::call_cpu_operation_t operation;
    const bool budget_exhausted = irq::detail::drain_bounded_interrupt_work(
        max_call_operations_per_softirq_batch, operation,
        [&cpu](auto &dequeued) { return cpu.try_dequeue_call_cpu(dequeued); },
        [](const auto &dequeued) { dequeued.func(dequeued.data); });
    if (budget_exhausted)
        irq::raise_soft_irq(irq::soft_vector::high_task);
}
} // namespace

irq::request_result flush_tlb_irq(const irq::interrupt_info *, u64) noexcept
{
    const auto generation = requested_tlb_flush_generation.load(std::memory_order_acquire);
    arch::paging::page_table_t::reload();
    acknowledged_tlb_flush_generation[cpu::current().id()].store(generation, std::memory_order_release);
    return irq::request_result::ok;
}

irq::request_result ipi_call(const irq::interrupt_info *, u64) noexcept
{
    // IPI_call is vector 250, above the timer priority class. Keep this hard
    // handler constant-time so a busy cross-CPU queue cannot starve local
    // timer delivery; bounded queue work runs through high_task after EOI.
    irq::raise_soft_irq(irq::soft_vector::high_task);
    return irq::request_result::ok;
}

void init()
{
    if (cpu::current().is_bsp())
    {
        KLOG_INFO("SMP init");
    }
    arch::SMP::init();
    if (cpu::current().is_bsp())
    {
        tlb_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
        call_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
        *tlb_registration = irq::register_handler(irq::hard_vector::IPI_tlb, irq::hard_handler::bind<&flush_tlb_irq>());
        *call_registration = irq::register_handler(irq::hard_vector::IPI_call, irq::hard_handler::bind<&ipi_call>());
        call_soft_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
        *call_soft_registration =
            irq::register_soft_handler(irq::soft_vector::high_task, irq::soft_handler::bind<&drain_call_cpu_queue>());
    }
}

void flush_all_tlb()
{
    const u32 current_id = cpu::current().id();
    const u64 generation = requested_tlb_flush_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    arch::APIC::local_post_IPI_all_notself(irq::hard_vector::IPI_tlb);

    // Do not release a physical mapping until every other CPU has processed
    // the TLB IPI. A stale translation is still an active access even after
    // the source page table has been edited.
    for (u32 id = 0; id < cpu::count(); ++id)
    {
        if (id == current_id)
            continue;
        while (acknowledged_tlb_flush_generation[id].load(std::memory_order_acquire) < generation)
            cpu_pause();
    }
}

void reschedule_cpu(u32 cpuid)
{
    arch::APIC::local_post_IPI_mask(irq::hard_vector::IPI_reschedule, arch::cpu::get(cpuid).get_apic_id());
}

void call_cpu(u32 cpuid, cpu::call_cpu_func_t call_func, u64 user_data)
{
    auto &cpu = cpu::get(cpuid);
    const cpu::call_cpu_operation_t operation{call_func, user_data};
    for (;;)
    {
        const auto result = cpu.enqueue_call_cpu(operation);
        if (result != cpu::call_cpu_enqueue_result::full)
        {
            if (result == cpu::call_cpu_enqueue_result::queued_needs_ipi)
                arch::APIC::local_post_IPI_mask(irq::hard_vector::IPI_call, arch::cpu::get(cpuid).get_apic_id());
            return;
        }
        cpu_pause();
    }
}

bool drain_msi_vector(u32 cpuid, u8 vector)
{
    constexpr u8 first_msi_vector = irq::vector_pool::first_msi_vector;
    if (cpuid >= arch::cpu::count() || vector < first_msi_vector ||
        (cpuid == arch::cpu::id() && !arch::idt::is_enable()))
        return false;
    auto &slot = msi_drain_slots[cpuid];
    if (slot.quarantined.load(std::memory_order_acquire))
        return false;

    auto &operation_lock = slot.operation_lock;
    u32 lock_polls = 0;
    while (operation_lock.test_and_set(std::memory_order_acquire))
    {
        if (++lock_polls == max_msi_drain_polls)
            return false;
        cpu_pause();
    }

    if (slot.quarantined.load(std::memory_order_acquire))
    {
        operation_lock.clear(std::memory_order_release);
        return false;
    }

    const u64 apic_id = arch::cpu::get(cpuid).get_apic_id();
    if (apic_id > 0xff)
    {
        operation_lock.clear(std::memory_order_release);
        return false;
    }

    slot.requested_vector.store(vector, std::memory_order_relaxed);
    u64 generation = slot.requested_generation.load(std::memory_order_relaxed) + 1;
    if (generation == 0)
        generation = 1;
    slot.requested_generation.store(generation, std::memory_order_release);
    arch::APIC::local_post_IPI_mask(irq::hard_vector::interrupt_drain, apic_id);

    u32 acknowledgement_polls = 0;
    while (slot.acknowledged_generation.load(std::memory_order_acquire) != generation)
    {
        if (++acknowledgement_polls == max_msi_drain_polls)
        {
            // An IPI may still arrive later. Never reuse this CPU's generation
            // slot after timing out, or a delayed handler could acknowledge a
            // different vector's drain request.
            slot.quarantined.store(true, std::memory_order_release);
            operation_lock.clear(std::memory_order_release);
            return false;
        }
        cpu_pause();
    }

    operation_lock.clear(std::memory_order_release);
    return true;
}

void handle_interrupt_drain_ipi()
{
    const u32 cpuid = arch::cpu::id();
    if (cpuid >= arch::cpu::max_cpu_support)
        return;
    auto &slot = msi_drain_slots[cpuid];
    const u64 generation = slot.requested_generation.load(std::memory_order_acquire);
    if (generation == 0 || slot.acknowledged_generation.load(std::memory_order_relaxed) == generation)
        return;

    const u8 vector = slot.requested_vector.load(std::memory_order_relaxed);
    for (u32 polls = 0; polls < max_msi_drain_polls; ++polls)
    {
        arch::idt::enable();
        const bool pending = arch::APIC::local_vector_pending_or_in_service(vector);
        arch::idt::disable();
        if (!pending && !arch::APIC::local_vector_pending_or_in_service(vector))
        {
            slot.acknowledged_generation.store(generation, std::memory_order_release);
            return;
        }
        cpu_pause();
    }
}

} // namespace SMP
