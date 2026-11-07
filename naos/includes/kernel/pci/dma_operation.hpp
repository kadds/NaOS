#pragma once

#include "naos/abi.h"

namespace pci
{
struct dma_operation_result
{
    constexpr dma_operation_result(na_status_t status, bool outcome_unknown = false) noexcept
        : status(status)
        , outcome_unknown(outcome_unknown)
    {
    }

    na_status_t status;
    bool outcome_unknown;
};
} // namespace pci
