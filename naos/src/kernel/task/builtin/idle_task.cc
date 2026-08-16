#include "kernel/task/builtin/idle_task.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/smp.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/init_task.hpp"
#include "kernel/task/builtin/input_task.hpp"
#include "kernel/task/builtin/soft_irq_task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::idle
{
namespace
{
bool move_bootstrap_capability(process_t &source, process_t &destination, na_handle_t &source_handle,
                               na_handle_t &destination_handle)
{
    if (source_handle == NA_HANDLE_INVALID || destination_handle != NA_HANDLE_INVALID)
        return false;

    na_resource_disposition_t disposition{};
    disposition.handle = source_handle;
    disposition.operation = NA_RESOURCE_MOVE;
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    if (source.resource.take_native_batch(&disposition, 1, NA_HANDLE_INVALID, records) != NA_STATUS_OK)
        return false;

    freelibcxx::vector<na_handle_t> destination_handles(memory::KernelCommonAllocatorV);
    auto status = destination.resource.reserve_native(destination_handles, 1);
    if (status == NA_STATUS_OK)
        status = destination.resource.activate_native(destination_handles[0], std::move(records[0].resource));
    if (status != NA_STATUS_OK)
    {
        destination.resource.rollback_native(destination_handles);
        (void)source.resource.restore_native_batch(records);
        return false;
    }

    destination_handle = destination_handles[0];
    source.resource.commit_native_batch(records);
    source_handle = NA_HANDLE_INVALID;
    return true;
}
} // namespace

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
        naos::ipc::init_kernel_dispatch_worker();

        auto file = fs::vfs::open("/bin/init", fs::vfs::global_root, fs::vfs::global_root,
                                  fs::mode::read | fs::mode::bin, fs::path_walk_flags::file);
        if (!file)
            trace::panic("Can't open init program");

        auto *init_process =
            task::create_process(file, "/bin/init", init::main, 0, 0, create_process_flags::deferred_start);
        if (init_process == nullptr)
            trace::panic("Can't create init process");

        // These are one-shot bootstrap authorities. Move them into the first
        // user process before it can issue its bootstrap syscall; later
        // hand-offs use ordinary capability MOVE dispositions.
        auto *kernel_process = task::current_process();
        if (kernel_process == nullptr)
            trace::panic("Unable to transfer initial device capabilities");
        init_process->bootstrap_capability_count = kernel_process->bootstrap_capability_count;
        for (uint32_t i = 0; i < kernel_process->bootstrap_capability_count; i++)
        {
            init_process->bootstrap_capabilities[i].kind = kernel_process->bootstrap_capabilities[i].kind;
            if (!move_bootstrap_capability(*kernel_process, *init_process,
                                           kernel_process->bootstrap_capabilities[i].handle,
                                           init_process->bootstrap_capabilities[i].handle))
                trace::panic("Unable to transfer initial device capabilities");
        }
        kernel_process->bootstrap_capability_count = 0;

        if (task::setsid(init_process) < 0)
            trace::warning("Unable to create init session");

        set_init_process(init_process);
        task::start_process(init_process);
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
