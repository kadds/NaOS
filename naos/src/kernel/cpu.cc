#include "kernel/cpu.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace cpu
{
bool cpu_data_t::is_bsp() { return arch::cpu::get(smp_id).is_bsp(); }

u32 cpu_data_t::id() { return smp_id; }

void cpu_data_t::set_task(task::thread_t *task)
{
    arch::cpu::current().set_context(task->kernel_stack_top);
    current_task = task;
}

call_cpu_enqueue_result cpu_data_t::enqueue_call_cpu(const call_cpu_operation_t &operation)
{
    uctx::RawSpinLockUninterruptibleContext ctx(call_cpu_queue_lock);
    if (call_cpu_queue_size == call_cpu_queue_capacity)
        return call_cpu_enqueue_result::full;

    const bool needs_ipi = call_cpu_queue_size == 0;
    call_cpu_queue[call_cpu_queue_tail] = operation;
    call_cpu_queue_tail = (call_cpu_queue_tail + 1) % call_cpu_queue_capacity;
    call_cpu_queue_size++;
    return needs_ipi ? call_cpu_enqueue_result::queued_needs_ipi : call_cpu_enqueue_result::queued;
}

bool cpu_data_t::try_dequeue_call_cpu(call_cpu_operation_t &operation)
{
    uctx::RawSpinLockUninterruptibleContext ctx(call_cpu_queue_lock);
    if (call_cpu_queue_size == 0)
        return false;

    operation = call_cpu_queue[call_cpu_queue_head];
    call_cpu_queue_head = (call_cpu_queue_head + 1) % call_cpu_queue_capacity;
    call_cpu_queue_size--;
    return true;
}

bool just_init = false;

bool has_init() { return just_init; }

cpu_data_t &current() { return *(cpu_data_t *)arch::cpu::current_user_data(); }

void init()
{
    kassert(arch::cpu::current().get_user_data() == nullptr, "Arch cpu data must be empty");
    cpu_data_t *cpu = memory::New<cpu_data_t>(memory::KernelCommonAllocatorV);
    arch::cpu::current().set_user_data(cpu);
    just_init = true;

    cpu->smp_id = arch::cpu::id();
    cpu->soft_irq_wait_queue = memory::New<task::wait_queue_t>(memory::KernelCommonAllocatorV);

    auto &c = arch::cpu::current();
    trace::debug("[CPU", c.get_id(), "] exception:", trace::hex(c.get_exception_rsp()),
                 " interrupt:", trace::hex(c.get_interrupt_rsp()), " kernel:", trace::hex(c.get_kernel_rsp()),
                 " data: ", trace::hex(cpu));
}

u64 count() { return arch::cpu::count(); }

cpu_data_t &get(u32 id) { return *(cpu_data_t *)arch::cpu::get(id).get_user_data(); }
} // namespace cpu
