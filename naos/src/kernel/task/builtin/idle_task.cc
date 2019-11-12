#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/init_task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::idle
{
void main(const kernel_start_args *args)
{
    trace::info("idle task running.");
    auto file = fs::vfs::open("/bin/init", fs::mode::read | fs::mode::bin, 0);
    task::create_process(file, init::main, 0, 0, 0, 0);
    // fs::vfs::close(file);
    trace::debug("init task has forked.");
    trace::debug("idle hlt.");
    task::schedule();
    while (1)
        __asm__ __volatile__("hlt\n\t" : : : "memory");
}
} // namespace task::builtin::idle
