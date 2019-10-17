#include "kernel/idle_task.hpp"
#include "kernel/init_task.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::idle
{
void main(const kernel_start_args *args)
{
    trace::info("idle task running.");
    task::do_fork(&task::builtin::init::main, 0, 0);
    scheduler::schedule();
    trace::debug("init task has forked.");
    trace::debug("idle hlt.");

    while (1)
        __asm__ __volatile__("hlt\n\t" : : : "memory");
}
} // namespace task::builtin::idle
