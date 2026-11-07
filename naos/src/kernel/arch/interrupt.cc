#include "kernel/arch/interrupt.hpp"
#include "freelibcxx/linked_list.hpp"
#include "kernel/arch/apic.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/gdt.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/irq.hpp"
#include "kernel/irq/soft_irq_drain.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/ucontext.hpp"
KLOG_MODULE(arch);
extern volatile char interrupt_code_end[], interrupt_code_start[];

namespace irq
{
namespace
{
u32 irq_context_depth[arch::cpu::max_cpu_support]{};
}

bool in_irq_context()
{
    if (!arch::cpu::has_init())
        return true;
    const u32 cpu_id = arch::cpu::current().get_id();
    return cpu_id >= arch::cpu::max_cpu_support || irq_context_depth[cpu_id] != 0;
}

struct irq_context_guard
{
    bool active;
    u32 cpu;
    irq_context_guard()
        : active(arch::cpu::has_init())
        , cpu(active ? arch::cpu::current().get_id() : 0)
    {
        if (active && cpu < arch::cpu::max_cpu_support)
            irq_context_depth[cpu]++;
        else
            active = false;
    }
    ~irq_context_guard()
    {
        if (active)
            irq_context_depth[cpu]--;
    }
};
} // namespace irq

namespace arch::interrupt
{
arch::idt::call_func global_call_func = 0;
arch::idt::call_func global_soft_irq_func = 0;
const int num_of_interrupt = 256 - 32;
void *code_buffer;
int a_code_len;

void build(int index, u8 ist)
{
    byte *buffer_start = (byte *)code_buffer + a_code_len * index;
    memcpy((void *)buffer_start, (void *)interrupt_code_start, a_code_len);

    *(u8 *)&buffer_start[3] = (u8)index + 32;
    idt::set_interrupt_system_gate(index + 32, buffer_start, ist);
}

void set_callback(arch::idt::call_func func) { global_call_func = func; }

void set_soft_irq_callback(arch::idt::call_func func) { global_soft_irq_func = func; }

void init()
{
    a_code_len = (u64)interrupt_code_end - (u64)interrupt_code_start;
    a_code_len = (a_code_len + 7) & ~(7);

    code_buffer = memory::KernelBuddyAllocatorV->allocate(a_code_len * num_of_interrupt, 8);
    for (int i = 0; i < num_of_interrupt; i++)
    {
        build(i, 0);
    }
    KLOG_INFO("Interrupt entry address: {} size: {}", code_buffer, a_code_len);
}

ExportC void on_intr_exit(const regs_t *regs) {}

ExportC _ctx_interrupt_ void do_irq(const regs_t *regs)
{
    irq::irq_context_guard irq_depth_guard;
    ::task::disable_preempt();
    if (regs->vector == irq::hard_vector::interrupt_drain)
    {
        // This reserved vector has a dedicated path: EOI first, then inspect
        // the MSI vector with interrupts temporarily enabled. It must not run
        // under the generic IRQ callback-list lock because the target MSI can
        // nest while being drained.
        APIC::EOI(regs->vector);
        SMP::handle_interrupt_drain_ipi();
    }
    else
    {
        if (likely(global_call_func))
            global_call_func(regs, 0);
        APIC::EOI(regs->vector);
    }
    kassert(!idt::is_enable(), "Should never enable interrupt");
    // void *intr_stack = cpu::current().get_interrupt_rsp();

    if (likely(global_soft_irq_func))
    {
        // kassert(intr_stack ==
        //             (void *)((get_stack() + memory::interrupt_stack_size - 1) & ~(memory::interrupt_stack_size - 1)),
        //         "stack error");
        //  void *soft_rsp = cpu::current().get_soft_irq_rsp();
        if (!cpu::current().in_soft_irq())
        {
            cpu::current().enter_soft_irq();
            // Soft handlers already run under their registration lock with
            // interrupts masked. Keep them masked between handlers too, so a
            // one-shot IRQ cannot requeue work after the final drain but
            // before in_soft_irq is cleared.
            global_soft_irq_func(regs, 0);
            irq::detail::drain_soft_interrupts_at_exit_barrier([] { idt::disable(); },
                                                               [] { return cpu::current().has_irq_pending(); },
                                                               [regs] { global_soft_irq_func(regs, 0); });
            cpu::current().exit_soft_irq();
        }
    }
    ::task::enable_preempt();
}

ExportC _ctx_interrupt_ void __do_irq(const regs_t *regs)
{
    irq::irq_context_guard irq_depth_guard;
    kassert(!idt::is_enable(), "Should never enable interrupt");

    if (!cpu::current().is_in_interrupt_context())
    {
        void *intr_stack = cpu::current().get_interrupt_rsp();
        _switch_stack((u64)regs, 0, 0, 0, (void *)do_irq, intr_stack);
    }
    else
    {
        do_irq(regs);
    }
    kassert(!idt::is_enable(), "Should never enable interrupt");
}

} // namespace arch::interrupt
