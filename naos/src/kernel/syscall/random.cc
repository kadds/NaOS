#include "kernel/arch/cpu_info.hpp"
#include "kernel/errno.hpp"
#include "kernel/syscall.hpp"
#include "kernel/usercopy.hpp"
#include "naos/abi.h"

namespace naos::syscall
{
namespace
{
bool try_hardware_random(u64 &value, bool rdseed_only)
{
    return rdseed_only ? arch::cpu_info::try_rdseed(value)
                       : arch::cpu_info::try_rdseed(value) || arch::cpu_info::try_rdrand(value);
}

bool has_hardware_random(bool rdseed_only)
{
    return rdseed_only ? arch::cpu_info::has_feature(arch::cpu_info::feature::rdseed)
                       : arch::cpu_info::has_feature(arch::cpu_info::feature::rdseed) ||
                             arch::cpu_info::has_feature(arch::cpu_info::feature::rdrand);
}
} // namespace

int getrandom(void *buffer, u64 length, u32 flags)
{
    constexpr u32 supported_flags =
        NA_GETRANDOM_FLAG_NONBLOCK | NA_GETRANDOM_FLAG_RANDOM | NA_GETRANDOM_FLAG_INSECURE | NA_GETRANDOM_FLAG_RDSEED;
    if ((flags & ~supported_flags) != 0)
        return EINVAL;
    if (length == 0)
        return 0;
    if (!is_user_space_range(buffer, length))
        return EFAULT;
    const bool rdseed_only = (flags & NA_GETRANDOM_FLAG_RDSEED) != 0;
    if (!has_hardware_random(rdseed_only))
        return ENOTSUP;

    u64 offset = 0;
    while (offset < length)
    {
        u64 value = 0;
        if (!try_hardware_random(value, rdseed_only))
            return EAGAIN;

        const u64 copy_length = length - offset < sizeof(value) ? length - offset : sizeof(value);
        if (naos::usercopy::copy_to(reinterpret_cast<u64>(buffer) + offset, &value, copy_length) != NA_STATUS_OK)
            return EFAULT;
        offset += copy_length;
    }
    return OK;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_GETRANDOM, getrandom)
END_SYSCALL
} // namespace naos::syscall
