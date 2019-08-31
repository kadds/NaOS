#include "kernel/init_task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::init
{
void main(u64 args) { trace::info("init task running."); }
} // namespace task::builtin::init
