#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/irq.hpp"
#include "kernel/task.hpp"
#include "kernel/wait.hpp"
namespace task::builtin::softirq
{
void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    while (1)
    {
        irq::wakeup_soft_irq_daemon();
    }
}
} // namespace task::builtin::softirq
