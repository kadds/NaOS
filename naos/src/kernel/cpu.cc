#include "kernel/cpu.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace cpu
{
bool cpu_data_t::is_bsp() { return arch::cpu::current().is_bsp(); }

int cpu_data_t::id() { return arch::cpu::current().get_id(); }

void cpu_data_t::set_task(task::thread_t *task)
{
    arch::cpu::current().set_context(task->kernel_stack_top);
    current_task = task;
}

cpu_data_t &current() { return *(cpu_data_t *)arch::cpu::current_user_data(); }

void init()
{
    kassert(arch::cpu::current().get_user_data() == nullptr, "arch cpu data must be nullptr");
    cpu_data_t *cpu = memory::New<cpu_data_t>(memory::KernelCommonAllocatorV);
    arch::cpu::current().set_user_data(cpu);

#ifdef _DEBUG
    auto &c = arch::cpu::current();
    trace::debug("[cpu", c.get_id(), "] exception rsp:", (void *)c.get_exception_rsp(),
                 " interrupt rsp:", (void *)c.get_interrupt_rsp(), " kernel rsp:", (void *)c.get_kernel_rsp());
#endif
}
} // namespace cpu
