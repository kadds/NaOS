#include "kernel/task/builtin/init_task.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/input_task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::init
{

void main(u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    trace::info("init task running.");
    task::create_thread(task::current_process(), task::builtin::input::main, 0, 0, 0, 0);
    arch::task::enter_userland(current(), (void *)arg3, 0);
}
} // namespace task::builtin::init
