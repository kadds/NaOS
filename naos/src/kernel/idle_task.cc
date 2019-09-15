#include "kernel/idle_task.hpp"
#include "kernel/init_task.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::idle
{
void main(const kernel_start_args *args)
{
    trace::info("idle task running.");
    task::do_fork(&task::builtin::init::main, (u64)(args->get_rfs_ptr()), 0);
    task::switch_task(current(), task::find_pid(1));
    while (1)
        ;
}
} // namespace task::builtin::idle
