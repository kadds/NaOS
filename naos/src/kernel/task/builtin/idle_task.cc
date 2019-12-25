#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/init_task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::idle
{
void main()
{
    kassert(arch::idt::is_enable(), "Disable IF");

    trace::debug("idle task running.");
    if (cpu::current().is_bsp())
    {
        auto file = fs::vfs::open("/bin/init", fs::vfs::global_root, fs::vfs::global_root,
                                  fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
        if (!file)
        {
            trace::panic("Can't open init");
        }
        task::create_process(file, init::main, 0, 0, 0, 0);
        // fs::vfs::close(file);
        kassert(arch::idt::is_enable(), "BSP IF ");
    }

    task::schedule();
    while (1)
    {
        kassert(arch::idt::is_enable(), "No");
        __asm__ __volatile__("pause\n\t" : : : "memory");
    }
}
} // namespace task::builtin::idle
