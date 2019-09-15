#include "kernel/init_task.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::init
{

void main(u64 args)
{
    trace::info("init task running.");
    exec_segment exec;
    exec.start_offset = args;
    exec.length = 0x1000;

    task::do_exec(exec, 0, 0);
    for (;;)
        ;
}
} // namespace task::builtin::init
