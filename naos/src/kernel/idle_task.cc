#include "kernel/idle_task.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::idle
{
void main(u64 args)
{
    trace::debug("idle task running.");
    // task::start_init_task();
    while (1)
        ;
}
} // namespace task::builtin::idle
