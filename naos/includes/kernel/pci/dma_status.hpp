#pragma once

#include "kernel/errno.hpp"
#include "naos/abi.h"

namespace pci::dma
{
constexpr int status_errno(na_status_t status) noexcept
{
    switch (status)
    {
        case NA_STATUS_OK:
            return 0;
        case NA_STATUS_ACCESS_DENIED:
            return EACCES;
        case NA_STATUS_INVALID_ARGUMENT:
            return EINVAL;
        case NA_STATUS_INVALID_HANDLE:
            return EBADF;
        case NA_STATUS_NOT_SUPPORTED:
            return ENOTSUP;
        case NA_STATUS_RESOURCE_EXHAUSTED:
            return ENOMEM;
        case NA_STATUS_WOULD_BLOCK:
            return EAGAIN;
        case NA_STATUS_OBJECT_REVOKED:
            return EIDRM;
        default:
            return EIO;
    }
}
} // namespace pci::dma
