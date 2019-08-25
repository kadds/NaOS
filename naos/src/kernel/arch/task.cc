#include "kernel/arch/task.hpp"
#include "kernel/trace.hpp"
namespace task
{
ExportC void switch_task(register_info_t *prev, register_info_t *next)
{
    trace::debug("prev task rip: ", (void *)prev->rip, ", next task rip: ", (void *)next->rip, ".");
}

} // namespace task
