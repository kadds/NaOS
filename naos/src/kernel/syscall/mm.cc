#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

namespace syscall
{
bool brk(u64 ptr)
{
    if (!is_user_space_pointer(ptr))
    {
        return false;
    }
    auto info = (memory::vm::info_t *)(task::current_process()->mm_info);
    return info->set_brk(ptr);
}

u64 sbrk(i64 offset)
{
    auto info = (memory::vm::info_t *)(task::current_process()->mm_info);
    if (offset == 0)
    {
        return info->get_brk();
    }
    auto r = info->get_brk();
    info->set_brk(r + offset);
    return r;
}

u64 map(u64 map_address, file_desc fd, u64 offset, u64 length)
{
    if (!is_user_space_pointer(map_address))
    {
        return 0;
    }
    // ((memory::vm::info_t *)(task::current_process()->mm_info))->mmu_paging.map_area();
}

u64 umap(u64 address)
{
    if (!is_user_space_pointer(address))
    {
        return 0;
    }
}

BEGIN_SYSCALL

SYSCALL(50, brk)
SYSCALL(51, sbrk)
SYSCALL(52, map)
SYSCALL(53, umap)

END_SYSCALL
} // namespace syscall
