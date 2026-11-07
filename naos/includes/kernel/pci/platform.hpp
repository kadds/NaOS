#pragma once

#include "kernel/pci/dma_operation.hpp"
#include "kernel/pci/msi_route.hpp"
#include "kernel/types.hpp"

template <typename T> class handle_t;

namespace pci
{
class function;
class discovery_fabric;
class dma_domain;
class dma_fault_source;
struct function_identity;
struct root_binding;

/// Platform supplied operations required by PCI capabilities. The PCI core
/// consumes this contract without depending on a firmware implementation.
struct platform_operations
{
    handle_t<discovery_fabric> (*get_discovery_fabric)() = nullptr;
    bool (*read_config)(const root_binding &, const function_identity &, u16, u8, u32 &) = nullptr;
    dma_operation_result (*create_dma_domain)(const function_identity &, process_id, handle_t<dma_domain> &,
                                              handle_t<dma_fault_source> &) = nullptr;
    bool (*prepare_route)(const function &, u8, msi_route &, interrupt_route_config_snapshot &, msi_config_error &,
                          bool &) = nullptr;
    bool (*set_route_enabled)(const function_identity &, const msi_route &, const interrupt_route_config_snapshot &,
                              bool, msi_config_error &, bool &) = nullptr;
    msi_route_teardown_result (*shutdown_route)(const function_identity &, const msi_route &,
                                                const interrupt_route_config_snapshot &) = nullptr;
};

/// Install the immutable platform implementation once during firmware setup.
bool install_platform_operations(const platform_operations &operations) noexcept;

handle_t<discovery_fabric> get_discovery_fabric();
bool read_config(const root_binding &binding, const function_identity &identity, u16 offset, u8 width, u32 &value);
dma_operation_result create_dma_domain(const function_identity &identity, process_id owner,
                                       handle_t<dma_domain> &result, handle_t<dma_fault_source> &fault_source);

bool prepare_interrupt_route(const function &pci_function, u8 vector, msi_route &route,
                             interrupt_route_config_snapshot &previous, msi_config_error &failure,
                             bool &outcome_uncertain);
bool set_interrupt_route_enabled(const function_identity &identity, const msi_route &route,
                                 const interrupt_route_config_snapshot &previous, bool enabled,
                                 msi_config_error &failure, bool &outcome_uncertain);
msi_route_teardown_result shutdown_interrupt_route(const function_identity &identity, const msi_route &route,
                                                   const interrupt_route_config_snapshot &previous);

} // namespace pci
