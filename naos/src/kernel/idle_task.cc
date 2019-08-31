#include "kernel/idle_task.hpp"
#include "kernel/init_task.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::idle
{
void main(u64 args)
{
    trace::info("idle task running.");
    task::do_fork(&task::builtin::init::main, 0, 0);
    task::switch_task(current(), task::find_pid(1));
    while (1)
        ;
}
} // namespace task::builtin::idle
