#include "kernel/init_task.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::init
{

void main(u64 args)
{
    trace::info("init task running.");
    task::do_exec("/bin/init", 0, 0, 0);

    for (;;)
        ;
}
} // namespace task::builtin::init
