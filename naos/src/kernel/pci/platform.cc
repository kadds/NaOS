#include "kernel/pci/platform.hpp"

#include "kernel/pci/discovery_fabric.hpp"
#include "kernel/pci/dma_domain.hpp"
#include "kernel/pci/dma_fault_source.hpp"
#include <atomic>

namespace pci
{
namespace
{
std::atomic<const platform_operations *> installed_operations{nullptr};
}

bool install_platform_operations(const platform_operations &operations) noexcept
{
    if (operations.get_discovery_fabric == nullptr || operations.read_config == nullptr ||
        operations.create_dma_domain == nullptr || operations.prepare_route == nullptr ||
        operations.set_route_enabled == nullptr || operations.shutdown_route == nullptr)
        return false;

    const platform_operations *expected = nullptr;
    if (installed_operations.compare_exchange_strong(expected, &operations, std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
        return true;
    return expected == &operations;
}

handle_t<discovery_fabric> get_discovery_fabric()
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    return operations == nullptr ? handle_t<discovery_fabric>() : operations->get_discovery_fabric();
}

bool read_config(const root_binding &binding, const function_identity &identity, u16 offset, u8 width, u32 &value)
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    return operations != nullptr && operations->read_config(binding, identity, offset, width, value);
}

dma_operation_result create_dma_domain(const function_identity &identity, process_id owner,
                                       handle_t<dma_domain> &result, handle_t<dma_fault_source> &fault_source)
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    if (operations == nullptr)
    {
        result.reset();
        fault_source.reset();
        return {NA_STATUS_NOT_SUPPORTED, false};
    }
    return operations->create_dma_domain(identity, owner, result, fault_source);
}

bool prepare_interrupt_route(const function &pci_function, u8 vector, msi_route &route,
                             interrupt_route_config_snapshot &previous, msi_config_error &failure,
                             bool &outcome_uncertain)
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    if (operations == nullptr)
    {
        failure = msi_config_error::route_unavailable;
        outcome_uncertain = false;
        return false;
    }
    return operations->prepare_route(pci_function, vector, route, previous, failure, outcome_uncertain);
}

bool set_interrupt_route_enabled(const function_identity &identity, const msi_route &route,
                                 const interrupt_route_config_snapshot &previous, bool enabled,
                                 msi_config_error &failure, bool &outcome_uncertain)
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    if (operations == nullptr)
    {
        failure = msi_config_error::route_unavailable;
        outcome_uncertain = false;
        return false;
    }
    return operations->set_route_enabled(identity, route, previous, enabled, failure, outcome_uncertain);
}

msi_route_teardown_result shutdown_interrupt_route(const function_identity &identity, const msi_route &route,
                                                   const interrupt_route_config_snapshot &previous)
{
    const auto *operations = installed_operations.load(std::memory_order_acquire);
    if (operations == nullptr)
        return {msi_route_teardown_failure::invalid_operations, false};
    return operations->shutdown_route(identity, route, previous);
}

} // namespace pci
