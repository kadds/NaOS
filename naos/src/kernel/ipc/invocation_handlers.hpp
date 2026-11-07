#pragma once

#include "kernel/ipc/invocation.hpp"
#include "kernel/ipc/kernel_view_dispatch.hpp"
#include "kernel/types.hpp"

namespace task
{
struct process_t;
}

namespace naos::ipc
{
kernel_dispatch::kernel_view_route_table io_kernel_view_routes() noexcept;
kernel_dispatch::kernel_view_route_table process_kernel_view_routes() noexcept;
kernel_dispatch::kernel_view_route_table service_kernel_view_routes() noexcept;
kernel_dispatch::kernel_view_route_table firmware_root_kernel_view_routes() noexcept;
kernel_dispatch::kernel_view_route_table acpi_kernel_view_routes() noexcept;
kernel_dispatch::kernel_view_route_table pci_kernel_view_routes() noexcept;
} // namespace naos::ipc
