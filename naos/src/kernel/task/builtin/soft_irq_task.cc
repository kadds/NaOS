#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/irq.hpp"
#include "kernel/task.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::softirq
{
void main(thread_start_info_t *info)
{
    task::set_cpu_mask(task::current(), task::current_cpu_mask());
    while (1)
    {
        irq::wakeup_soft_irq_daemon();
    }
}
} // namespace task::builtin::softirq
