#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/dev/tty/tty.hpp"
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
void main(void *arg)
{
    trace::debug("idle task running at cpu ", cpu::current().id());
    if (cpu::current().is_bsp())
    {
        auto p = task::create_kernel_process(builtin::softirq::main, 0, create_thread_flags::real_time_rr);
        trace::debug("softirqd created tid=", p->main_thread->tid);
        kassert(p->pid == 1, "BUG check failed.");
        is_init = true;
        task::create_kernel_process(builtin::input::main, 0, create_thread_flags::real_time_rr);

        auto file = fs::vfs::open("/bin/init", fs::vfs::global_root, fs::vfs::global_root,
                                  fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
        if (!file)
            trace::panic("Can't open init program");

        auto *init_process = task::create_process(file, "/bin/init", init::main, 0, 0, 0);
        if (init_process == nullptr)
            trace::panic("Can't create init process");

        if (task::setsid(init_process) < 0)
            trace::warning("Unable to create init session");

        auto tty_file =
            fs::vfs::open("/dev/tty0", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read | fs::mode::write, 0);
        auto *tty = tty_file ? reinterpret_cast<dev::tty::tty_pseudo_t *>(tty_file->get_pseudo()) : nullptr;
        if (tty == nullptr || task::attach_controlling_tty(init_process, &tty->core()) != 0)
            trace::warning("Unable to attach init to /dev/tty0");

        set_init_process(init_process);
    }
    else
    {
        while (!is_init)
        {
            cpu_pause();
        }
        /// soft irq process
        auto t = task::create_thread(task::find_pid(1), builtin::softirq::main, nullptr, 0,
                                     create_thread_flags::real_time_rr);
        trace::debug("softirqd created tid=", t->tid);
    }

    task::thread_yield();
    while (1)
    {
        kassert(arch::idt::is_enable(), "Bug check failed.");
        cpu_halt();
    }
}
} // namespace task::builtin::idle
