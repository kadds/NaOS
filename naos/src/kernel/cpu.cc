#include "kernel/cpu.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace cpu
{
bool cpu_data_t::is_bsp() { return arch::cpu::get(smp_id).is_bsp(); }

u32 cpu_data_t::id() { return smp_id; }

void cpu_data_t::set_task(task::thread_t *task)
{
    arch::cpu::current().set_context(task->kernel_stack_top);
    current_task = task;
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
    trace::debug("[CPU", c.get_id(), "] exception rsp:", trace::hex(c.get_exception_rsp()),
                 " interrupt rsp:", trace::hex(c.get_interrupt_rsp()), " kernel rsp:", trace::hex(c.get_kernel_rsp()),
                 "\n cpu data: ", trace::hex(cpu));
}

u64 count() { return arch::cpu::count(); }

cpu_data_t &get(u32 id) { return *(cpu_data_t *)arch::cpu::get(id).get_user_data(); }
} // namespace cpu
