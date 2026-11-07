#pragma once

#include "kernel/types.hpp"

namespace pci
{
enum class msi_route_teardown_failure : u8
{
    none,
    invalid_operations,
    route_binding_invalid,
    destination_cpu_unavailable,
    disable_failed,
    pre_retirement_drain_failed,
    provider_route_retirement_failed,
    post_retirement_drain_failed,
    restore_failed,
};

struct msi_route_teardown_result
{
    msi_route_teardown_failure failure = msi_route_teardown_failure::none;
    bool outcome_uncertain = false;
    /// True once the source is disabled, the IRTE is retired, and both
    /// destination-vector drains complete. The vector can then be recycled
    /// even if restoring the function's original disabled configuration fails.
    bool vector_recyclable = false;
};

struct msi_route_teardown_ops
{
    bool (*disable)(void *context) = nullptr;
    bool (*drain)(void *context) = nullptr;
    bool (*retire_irte)(void *context) = nullptr;
    bool (*restore)(void *context) = nullptr;
    void *context = nullptr;
};

/// Enforce the route teardown order: disable device MSI/MSI-X production, drain
/// messages already accepted by the destination LAPIC, retire and IEC-invalidate
/// the IRTE, drain again, then restore the function's original configuration.
/// A failure before the second drain requires the caller to quarantine the
/// vector. After that drain, the vector is safe to recycle even if restoring
/// the function's original configuration fails. No later step is retried here.
inline msi_route_teardown_result teardown_msi_route(const msi_route_teardown_ops &operations) noexcept
{
    if (operations.disable == nullptr || operations.drain == nullptr || operations.retire_irte == nullptr ||
        operations.restore == nullptr)
        return {msi_route_teardown_failure::invalid_operations, false, false};

    if (!operations.disable(operations.context))
        return {msi_route_teardown_failure::disable_failed, true, false};
    if (!operations.drain(operations.context))
        return {msi_route_teardown_failure::pre_retirement_drain_failed, true, false};
    if (!operations.retire_irte(operations.context))
        return {msi_route_teardown_failure::provider_route_retirement_failed, true, false};
    if (!operations.drain(operations.context))
        return {msi_route_teardown_failure::post_retirement_drain_failed, true, false};
    if (!operations.restore(operations.context))
        return {msi_route_teardown_failure::restore_failed, true, true};

    return {msi_route_teardown_failure::none, false, true};
}
} // namespace pci
