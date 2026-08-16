#pragma once

#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/capability.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mutex.hpp"
#include "kernel/types.hpp"

namespace service
{

class directory final : public kobject
{
  public:
    directory();
    ~directory() override;

    static type_e type_of() { return type_e::service_directory; }

    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

    i64 register_service(const char *uri, u64 uri_size, capability::transfer_record &record, bool one_shot = false,
                         process_id owner = 0);
    i64 resolve_service(const char *uri, u64 uri_size, capability::transferred_resource &resource);
    i64 unregister_service(const char *uri, u64 uri_size, process_id owner = 0);
    i64 listen_service(const char *uri, u64 uri_size, khandle send_endpoint, khandle descriptor, u64 max_pending,
                       process_id owner = 0);
    i64 connect_service(const char *uri, u64 uri_size, const na_uuid_t &expected_uuid, u64 requested_rights,
                        u64 requested_revision, u64 requested_features,
                        capability::transferred_resource &client_resource, u64 &selected_revision,
                        u64 &selected_features);
    i64 list_services(u64 offset, u64 requested_bytes, freelibcxx::vector<byte> &records, u64 &next, u64 &count) const;
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
};

handle_t<directory> get_global_service_directory();
void set_global_service_directory(handle_t<directory> directory);
void register_kernel_service(const char *uri, u64 uri_size, khandle object, capability::metadata meta,
                             bool one_shot = false);

} // namespace service
