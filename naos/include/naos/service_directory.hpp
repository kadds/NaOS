#pragma once

#include "naos/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Locator naming rules:
 * - kernel/... names kernel-published typed endpoints, exposed through scoped directories.
 * - system/... names stable machine-wide services owned by system daemons.
 * - service/... names general service roles and boot resources; the prefix does not identify the provider.
 * Add an instance key only when multiple named instances must be discovered. Do not use /0 as a default
 * instance or protocol version; pass dynamic objects as capabilities.
 */
#define NAOS_SERVICE_TERMINAL_DRIVER_FACTORY "naos://service/terminal/driver"
#define NAOS_SERVICE_INPUT_EVENT_SOURCE "naos://service/input"
#define NAOS_SERVICE_FRAMEBUFFER "naos://service/framebuffer"
#define NAOS_SERVICE_VFS "naos://service/fs/vfs"
/* Kernel-published, one-shot executable images for the userland boot
 * manager.  These are generic boot-module resources; they are not passed as
 * service-specific bootstrap capabilities. */
#define NAOS_SERVICE_BOOT_MODULE_ROOTFSD "naos://service/boot/module/rootfsd"
#define NAOS_SERVICE_BOOT_MODULE_INIT "naos://service/boot/module/init"
#define NAOS_SERVICE_BOOT_ROOT_IMAGE "naos://service/boot/root-image"
#define NAOS_SERVICE_SYSTEM_STATUS "naos://service/system/status"
#define NAOS_SERVICE_FIRMWARE_ROOT "naos://kernel/firmware/root"
#define NAOS_SERVICE_PCI_DISCOVERY "naos://kernel/hw/pci-discovery"
/* Fixed key in a grant-private directory, not a globally named PCI function. */
#define NAOS_SERVICE_PCI_FUNCTION "naos://service/pci/function"
#define NAOS_SERVICE_ACPI_FIXED_HARDWARE "naos://kernel/hw/acpi-fixed"
#define NAOS_SERVICE_ACPI_RESOURCE_AUTHORITY "naos://kernel/hw/acpi-resource-authority"
#define NAOS_SERVICE_PLATFORM_PCI_TOPOLOGY "naos://system/platform/pci-topology"
#define NAOS_SERVICE_PLATFORM_POWER_CONTROL "naos://system/platform/power/control"
#define NAOS_SERVICE_PCI_BLOCK_DEVICE_CATALOG "naos://system/pci/block-device-catalog"

/* Registers and consumes handle on success; the handle remains owned by the caller on failure. */
int naos_service_register_handle(const char *uri, na_handle_t handle);
/* Registers a duplicate of fd's native handle, preserving the fd. */
int naos_service_register_fd(const char *uri, int fd);
int naos_service_resolve(const char *uri, na_handle_t *handle);
/* Registers a multi-client listener. listener must be a raw channel endpoint
 * whose peer stays with the provider; descriptor is a protocol descriptor. */
int naos_service_listen(const char *uri, na_handle_t listener, na_handle_t descriptor, uint64_t max_pending);
/* Connects to a registered listener and returns a fresh ClientEnd handle. */
int naos_service_connect(const char *uri, const na_uuid_t *expected_uuid, uint64_t requested_rights,
                         na_handle_t *client);
int naos_service_connect_versioned(const char *uri, const na_uuid_t *expected_uuid, uint64_t requested_rights,
                                   uint64_t requested_revision, uint64_t requested_features, na_handle_t *client);
int naos_service_unregister(const char *uri);
int naos_handle_close(na_handle_t handle);

#ifdef __cplusplus
}
#endif
