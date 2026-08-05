#include "kernel/usercopy.hpp"

#include "kernel/task.hpp"

namespace naos::usercopy
{
namespace
{
na_status_t copy_bytes(void *destination, const void *source, u64 size)
{
    if (size == 0)
        return NA_STATUS_OK;
    auto *thread = task::current();
    if (thread == nullptr)
        return NA_STATUS_FAULT;

    u64 remaining = size;
    auto *dst = static_cast<byte *>(destination);
    auto *src = static_cast<const byte *>(source);

    // `rep movsb` is deliberately in this function rather than delegated to
    // libc memcpy: its faulting instruction has this function's stack frame,
    // so the exception path can safely continue at copy_fault without having
    // to unwind an implementation-dependent memcpy frame.
    thread->usercopy_active = true;
    thread->usercopy_resume = reinterpret_cast<u64>(&&copy_fault);
    asm volatile("cld\n\t"
                 "rep movsb"
                 : "+D"(dst), "+S"(src), "+c"(remaining)
                 :
                 : "memory");
    thread->usercopy_active = false;
    thread->usercopy_resume = 0;
    return NA_STATUS_OK;

copy_fault:
    thread->usercopy_active = false;
    thread->usercopy_resume = 0;
    return NA_STATUS_FAULT;
}
} // namespace

na_status_t copy_from(void *destination, u64 source, u64 size)
{
    if (size == 0)
        return NA_STATUS_OK;
    if (destination == nullptr || !is_user_space_range(reinterpret_cast<const void *>(source), size))
        return NA_STATUS_FAULT;
    return copy_bytes(destination, reinterpret_cast<const void *>(source), size);
}

na_status_t copy_to(u64 destination, const void *source, u64 size)
{
    if (size == 0)
        return NA_STATUS_OK;
    if (source == nullptr || !is_user_space_range(reinterpret_cast<void *>(destination), size))
        return NA_STATUS_FAULT;
    return copy_bytes(reinterpret_cast<void *>(destination), source, size);
}

bool recover_page_fault(regs_t *regs)
{
    auto *thread = task::current();
    if (thread == nullptr || regs == nullptr || !thread->usercopy_active || thread->usercopy_resume == 0)
        return false;

    // A usercopy fault is necessarily raised while the CPU is executing the
    // kernel copy instruction.  Never redirect a genuine user-mode fault.
    if ((regs->cs & 0x3) != 0)
        return false;
    regs->rip = thread->usercopy_resume;
    return true;
}

} // namespace naos::usercopy
