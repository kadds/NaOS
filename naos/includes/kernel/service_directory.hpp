#pragma once

#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/capability.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mutex.hpp"
#include "kernel/types.hpp"
#include "naos/service_directory.hpp"

namespace task
{
struct process_t;
}

namespace service
{

inline constexpr char terminal_driver_factory_uri[] = NAOS_SERVICE_TERMINAL_DRIVER_FACTORY;
inline constexpr char input_event_source_uri[] = NAOS_SERVICE_INPUT_EVENT_SOURCE;
inline constexpr char framebuffer_uri[] = NAOS_SERVICE_FRAMEBUFFER;
inline constexpr char boot_module_rootfsd_uri[] = NAOS_SERVICE_BOOT_MODULE_ROOTFSD;
inline constexpr char boot_module_init_uri[] = NAOS_SERVICE_BOOT_MODULE_INIT;
inline constexpr char boot_root_image_uri[] = NAOS_SERVICE_BOOT_ROOT_IMAGE;
inline constexpr char system_status_uri[] = NAOS_SERVICE_SYSTEM_STATUS;
inline constexpr char firmware_root_uri[] = NAOS_SERVICE_FIRMWARE_ROOT;
inline constexpr char pci_discovery_uri[] = NAOS_SERVICE_PCI_DISCOVERY;
inline constexpr char acpi_fixed_hardware_uri[] = NAOS_SERVICE_ACPI_FIXED_HARDWARE;
inline constexpr char acpi_resource_authority_uri[] = NAOS_SERVICE_ACPI_RESOURCE_AUTHORITY;
inline constexpr char platform_pci_topology_uri[] = NAOS_SERVICE_PLATFORM_PCI_TOPOLOGY;
inline constexpr char platform_power_control_uri[] = NAOS_SERVICE_PLATFORM_POWER_CONTROL;
inline constexpr char pci_block_device_catalog_uri[] = NAOS_SERVICE_PCI_BLOCK_DEVICE_CATALOG;

class directory final : public kobject
{
  public:
    directory();
    ~directory() override;

    /// Create a read-only locator view over selected kernel-published URIs.
    /// Views are backed by the boot-lifetime global directory and cannot
    /// register, unregister, listen, or enumerate services.
    static handle_t<directory> make_scoped_view(const handle_t<directory> &parent, const char *const *uris,
                                                u64 uri_count);
    static handle_t<directory> make_scoped_provider_view(const handle_t<directory> &parent, const char *const *uris,
                                                         u64 uri_count, const char *const *publish_uris,
                                                         u64 publish_uri_count);
    /// Create an isolated directory that its owner may populate without
    /// publishing entries into the global ServiceDirectory.
    static handle_t<directory> make_private_registry(u64 max_entries);

    static constexpr type_e type_of() { return type_e::service_directory; }

    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

    i64 register_service(const char *uri, u64 uri_size, capability::transfer_record &record, bool one_shot = false,
                         process_id owner = 0);
    i64 resolve_service(const char *uri, u64 uri_size, capability::transferred_resource &resource);
    i64 unregister_service(const char *uri, u64 uri_size, process_id owner = 0);
    i64 listen_service(const char *uri, u64 uri_size, khandle send_endpoint, khandle descriptor, u64 max_pending,
                       process_id owner = 0);
    i64 connect_service(const char *uri, u64 uri_size, const na_uuid_t &expected_uuid, u64 requested_rights,
                        u64 requested_revision, u64 requested_features, ::task::process_t *caller,
                        capability::transferred_resource &client_resource, u64 &selected_revision,
                        u64 &selected_features);
    i64 validate_view_uri(const char *uri, u64 uri_size) const;
    bool allows_local_registration() const { return private_registry_; }
    bool allows_provider_publish(const char *uri, u64 uri_size) const
    {
        return static_cast<bool>(view_parent_) && permits_publish(uri, uri_size);
    }
    i64 list_services(const char *prefix, u64 prefix_size, u64 offset, u64 requested_bytes,
                      freelibcxx::vector<byte> &records, u64 &next, u64 &count) const;
    void cleanup_owner(process_id owner);

  private:
    struct entry
    {
        freelibcxx::string uri;
        khandle object;
        capability::metadata metadata;
        bool one_shot;
        process_id owner;

        entry(freelibcxx::string &&uri, khandle &&object, capability::metadata metadata, bool one_shot,
              process_id owner)
            : uri(std::move(uri))
            , object(std::move(object))
            , metadata(metadata)
            , one_shot(one_shot)
            , owner(owner)
        {
        }
    };

    struct listener_entry
    {
        freelibcxx::string uri;
        khandle send_endpoint;
        khandle descriptor;
        u64 max_pending;
        process_id owner;

        listener_entry(freelibcxx::string &&uri, khandle &&send_endpoint, khandle &&descriptor, u64 max_pending,
                       process_id owner)
            : uri(std::move(uri))
            , send_endpoint(std::move(send_endpoint))
            , descriptor(std::move(descriptor))
            , max_pending(max_pending)
            , owner(owner)
        {
        }
    };

    static bool valid_uri(const char *uri, u64 uri_size);
    static bool parent_permits_uri(const void *context, const char *uri, u64 uri_size);
    static bool parent_permits_publish_uri(const void *context, const char *uri, u64 uri_size);
    bool permits(const char *uri, u64 uri_size) const;
    bool permits_publish(const char *uri, u64 uri_size) const;
    i64 resolve_service_inner(const char *uri, u64 uri_size, capability::transferred_resource &resource,
                              bool scoped_view);
    i64 connect_service_inner(const char *uri, u64 uri_size, const na_uuid_t &expected_uuid, u64 requested_rights,
                              u64 requested_revision, u64 requested_features, task::process_t *caller,
                              capability::transferred_resource &client_resource, u64 &selected_revision,
                              u64 &selected_features, bool scoped_view);
    i64 find_locked(const char *uri, u64 uri_size) const;
    i64 find_listener_locked(const char *uri, u64 uri_size) const;
    void release_entry(entry &value);
    void release_listener(listener_entry &value);

    mutable lock::spinlock_t lock_;
    // Capacity growth may allocate.  Keep it outside lock_, but serialize
    // concurrent prepare phases so vector internals are never mutated by two
    // registrations at once.
    mutable lock::mutex_t allocation_lock_;
    freelibcxx::vector<entry> entries_;
    freelibcxx::vector<listener_entry> listeners_;
    handle_t<directory> view_parent_;
    freelibcxx::vector<freelibcxx::string> view_uris_;
    freelibcxx::vector<freelibcxx::string> view_publish_uris_;
    bool private_registry_ = false;
    u64 private_registry_entry_limit_ = 0;
};

handle_t<directory> get_global_service_directory();
void set_global_service_directory(handle_t<directory> directory);
i64 register_kernel_service(const char *uri, u64 uri_size, khandle object, capability::metadata meta,
                            bool one_shot = false);

} // namespace service
