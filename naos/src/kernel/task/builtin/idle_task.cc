#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/init_task.hpp"
#include "kernel/task/builtin/input_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::idle
{
std::atomic_bool is_init = false;
void main()
{
    trace::debug("idle task running at cpu ", cpu::current().id());
    if (cpu::current().is_bsp())
    {
        auto p = task::create_kernel_process(builtin::softirq::main, 0, create_thread_flags::real_time_rr);
        trace::debug("softirqd created tid=", p->main_thread->tid);
        is_init = true;
        task::create_kernel_process(builtin::input::main, 0, create_thread_flags::real_time_rr);

        auto file = fs::vfs::open("/bin/init", fs::vfs::global_root, fs::vfs::global_root,
                                  fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
        if (!file)
            trace::panic("Can't open init program");

        task::create_process(file, init::main, 0, 0);

        // fs::vfs::close(file);
    }
    else
    {
        while (!is_init)
        {
            cpu_pause();
        }
        auto t =
            task::create_thread(task::find_pid(1), builtin::softirq::main, 0, 0, 0, create_thread_flags::real_time_rr);
        trace::debug("softirqd created tid=", t->tid);
    }

    task::thread_yield();
    while (1)
    {
        kassert(arch::idt::is_enable(), "Bug check failed. interrupt disable");
        __asm__ __volatile__("pause\n\t" : : : "memory");
    }
}
} // namespace task::builtin::idle
