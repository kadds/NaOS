#include "kernel/task/builtin/init_task.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/input_task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::init
{

void main(task::thread_start_info_t *info)
{
    trace::info("init task running. pid ", task::current_process()->pid);
    u64 offset = info->userland_stack_offset;
    void *args = info->args;
    void *entry = info->userland_entry;
    memory::Delete(memory::KernelCommonAllocatorV, info);
    arch::task::enter_userland(current(), offset, entry, reinterpret_cast<u64>(args), 0);
}
} // namespace task::builtin::init
