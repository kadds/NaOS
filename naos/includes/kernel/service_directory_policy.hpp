#pragma once

#include "kernel/types.hpp"
#include "naos/service_directory.hpp"

namespace service
{
using uri_scope_contains = bool (*)(const void *context, const char *uri, u64 uri_size);

/// A child ServiceDirectory view must only name exact URIs accepted by its
/// parent view. This is an attenuation check; it does not grant access itself.
inline bool uri_allowlist_is_subset(const char *const *uris, u64 uri_count, uri_scope_contains parent_contains,
                                    const void *parent_context)
{
    if (uris == nullptr || uri_count == 0 || parent_contains == nullptr)
        return false;
    for (u64 index = 0; index < uri_count; ++index)
    {
        const char *uri = uris[index];
        if (uri == nullptr || !parent_contains(parent_context, uri, strlen(uri)))
            return false;
    }
    return true;
}

inline constexpr u64 bootstrap_service_directory_rights(u64 granted_rights) noexcept
{
    constexpr u64 delegable_rights = NA_SERVICE_DIRECTORY_RIGHT_ADMIN | NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER |
                                     NA_SERVICE_DIRECTORY_RIGHT_CREATE_VIEW |
                                     NA_SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER | NA_SERVICE_DIRECTORY_RIGHT_PROVIDER;
    return NA_PROTOCOL_RIGHT_INVOKE | (granted_rights & delegable_rights);
}

inline constexpr u64 bootstrap_service_directory_meta_rights(bool allow_transfer) noexcept
{
    constexpr u64 common_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    return common_rights | (allow_transfer ? NA_RIGHT_TRANSFER : 0);
}

inline bool requires_scoped_view_to_resolve(const char *uri, u64 uri_size)
{
    if (uri == nullptr)
        return false;
    const auto matches = [uri, uri_size](const char *expected, u64 expected_size) {
        return uri_size == expected_size && memcmp(uri, expected, static_cast<size_t>(expected_size)) == 0;
    };
    return matches(NAOS_SERVICE_FIRMWARE_ROOT, sizeof(NAOS_SERVICE_FIRMWARE_ROOT) - 1) ||
           matches(NAOS_SERVICE_ACPI_FIXED_HARDWARE, sizeof(NAOS_SERVICE_ACPI_FIXED_HARDWARE) - 1) ||
           matches(NAOS_SERVICE_ACPI_RESOURCE_AUTHORITY, sizeof(NAOS_SERVICE_ACPI_RESOURCE_AUTHORITY) - 1) ||
           matches(NAOS_SERVICE_PCI_DISCOVERY, sizeof(NAOS_SERVICE_PCI_DISCOVERY) - 1) ||
           matches(NAOS_SERVICE_PLATFORM_PCI_TOPOLOGY, sizeof(NAOS_SERVICE_PLATFORM_PCI_TOPOLOGY) - 1);
}

/// Sensitive kernel/platform services require a kernel-created, URI-scoped
/// ServiceDirectory view. Ordinary global-directory handles cannot resolve or
/// connect to those locators even if their caller has SYSTEM_MANAGER rights.
inline bool resolve_uri_allowed(const char *uri, u64 uri_size, bool scoped_view)
{
    return scoped_view || !requires_scoped_view_to_resolve(uri, uri_size);
}

inline bool connect_uri_allowed(const char *uri, u64 uri_size, bool scoped_view)
{
    return resolve_uri_allowed(uri, uri_size, scoped_view);
}
} // namespace service
