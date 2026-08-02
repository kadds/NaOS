#pragma once

#include "common.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include <type_traits>

namespace fs::vfs
{
/// Kernel-side view of the ioctl ABI. Immediate arguments are exposed through
/// value(), while pointer arguments must use the typed user-buffer helpers.
class ioctl_context final
{
  public:
    explicit ioctl_context(u64 request, u64 argument)
        : request_(request)
        , argument_(argument)
    {
    }

    u64 request() const { return request_; }
    u64 value() const { return argument_; }

    template <typename T> i64 read_user(T &output) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (argument_ == 0)
            return EINVAL;
        auto *input = reinterpret_cast<const T *>(argument_);
        if (!is_user_space_range(input, sizeof(T)))
            return EFAULT;
        output = *input;
        return OK;
    }

    template <typename T> i64 write_user(const T &input) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (argument_ == 0)
            return EINVAL;
        auto *output = reinterpret_cast<T *>(argument_);
        if (!is_user_space_range(output, sizeof(T)))
            return EFAULT;
        *output = input;
        return OK;
    }

  private:
    u64 request_;
    u64 argument_;
};
} // namespace fs::vfs
