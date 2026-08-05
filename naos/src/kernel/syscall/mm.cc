#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

namespace naos::syscall
{
u64 brk(u64 ptr)
{
    if (!is_user_space_pointer(ptr))
    {
        return EPARAM;
    }
    auto info = (memory::vm::info_t *)(task::current_process()->mm_info);
    if (info->set_brk(ptr))
    {
        return OK;
    }
    return EFAILED;
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

BEGIN_SYSCALL

SYSCALL(NA_SYSCALL_BRK, brk)
SYSCALL(NA_SYSCALL_SBRK, sbrk)

END_SYSCALL
} // namespace naos::syscall
