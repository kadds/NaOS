#include "kernel/task/builtin/init_task.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/task/builtin/input_task.hpp"
KLOG_MODULE(task);
namespace task::builtin::vfsd
{
void main(task::thread_start_info_t *info)
{
    KLOG_INFO("vfsd early service entering userland");
    u64 offset = info->userland_stack_offset;
    void *args = info->args;
    void *entry = info->userland_entry;
    memory::Delete(memory::KernelCommonAllocatorV, info);
    arch::task::enter_userland(current(), offset, entry, reinterpret_cast<u64>(args), 0);
}
} // namespace task::builtin::vfsd

namespace task::builtin::ramdiskd
{
void main(task::thread_start_info_t *info)
{
    KLOG_INFO("ramdiskd block backend entering userland");
    u64 offset = info->userland_stack_offset;
    void *args = info->args;
    void *entry = info->userland_entry;
    memory::Delete(memory::KernelCommonAllocatorV, info);
    arch::task::enter_userland(current(), offset, entry, reinterpret_cast<u64>(args), 0);
}
} // namespace task::builtin::ramdiskd
