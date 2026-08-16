#include "kernel/service_directory.hpp"

#include "kernel/errno.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"

namespace service
{
namespace
{
handle_control *global_service_directory_control = nullptr;
} // namespace

void release_table_root(khandle &value);

handle_t<directory> get_global_service_directory() { return handle_t<directory>(global_service_directory_control); }

void set_global_service_directory(handle_t<directory> directory)
{
    if (!directory)
        return;
    auto *control = directory.get_control();
    control->ref++;
    // The kernel keeps one reference for the lifetime of the system.
    global_service_directory_control = control;
}

void register_kernel_service(const char *uri, u64 uri_size, khandle object, capability::metadata meta, bool one_shot)
{
    auto directory = get_global_service_directory();
    if (!directory)
        return;
    capability::transferred_resource resource(object, meta);
    capability::transfer_record record(NA_HANDLE_INVALID, true, std::move(resource));
    (void)directory->register_service(uri, uri_size, record, one_shot);
}

directory::directory()
    : kobject(type_e::service_directory)
    , entries_(memory::KernelCommonAllocatorV)
    , listeners_(memory::KernelCommonAllocatorV)
{
}

directory::~directory()
{
    freelibcxx::vector<khandle> objects(memory::KernelCommonAllocatorV);
    freelibcxx::vector<khandle> endpoints(memory::KernelCommonAllocatorV);
    objects.ensure(entries_.size());
    endpoints.ensure(listeners_.size() * 2);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (auto &value : entries_)
            objects.push_back(std::move(value.object));
        entries_.clear();
        for (auto &value : listeners_)
        {
            endpoints.push_back(std::move(value.send_endpoint));
            endpoints.push_back(std::move(value.descriptor));
        }
        listeners_.clear();
    }
    for (auto &object : objects)
    {
        release_table_root(object);
    }
    for (auto &endpoint : endpoints)
    {
        release_table_root(endpoint);
    }
}

bool directory::valid_uri(const char *uri, u64 uri_size)
{
    constexpr char prefix[] = "naos://";
    constexpr u64 prefix_size = sizeof(prefix) - 1;
    if (uri == nullptr || uri_size <= prefix_size || uri_size > 255 ||
        memcmp(uri, prefix, static_cast<size_t>(prefix_size)) != 0)
        return false;
    bool segment_has_value = false;
    bool segment_is_dot = true;
    u64 segment_size = 0;
    for (u64 i = prefix_size; i < uri_size; i++)
    {
        const auto value = static_cast<unsigned char>(uri[i]);
        if (value == '/')
        {
            if (!segment_has_value || segment_is_dot || segment_size == 2)
                return false;
            segment_has_value = false;
            segment_is_dot = true;
            segment_size = 0;
            continue;
        }
        const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!alpha && !digit && value != '-' && value != '_' && value != '.' && value != '~')
            return false;
        segment_has_value = true;
        segment_size++;
        if (segment_size > 2 || value != '.')
            segment_is_dot = false;
    }
    return segment_has_value && !segment_is_dot && segment_size != 2;
}

i64 directory::find_locked(const char *uri, u64 uri_size) const
{
    const auto entries = entries_.cspan();
    for (u64 i = 0; i < entries_.size(); i++)
    {
        if (entries[i].uri.size() == uri_size && memcmp(entries[i].uri.data(), uri, static_cast<size_t>(uri_size)) == 0)
            return static_cast<i64>(i);
    }
    return -1;
}

i64 directory::find_listener_locked(const char *uri, u64 uri_size) const
{
    const auto listeners = listeners_.cspan();
    for (u64 i = 0; i < listeners_.size(); i++)
    {
        if (listeners[i].uri.size() == uri_size &&
            memcmp(listeners[i].uri.data(), uri, static_cast<size_t>(uri_size)) == 0)
            return static_cast<i64>(i);
    }
    return -1;
}

void release_table_root(khandle &value)
{
    if (value)
        value->on_capability_release(capability::location::table_root);
    value.reset();
}

void directory::release_entry(entry &value)
{
    release_table_root(value.object);
}

void directory::release_listener(listener_entry &value)
{
    release_table_root(value.send_endpoint);
    release_table_root(value.descriptor);
}

i64 directory::register_service(const char *uri, u64 uri_size, capability::transfer_record &record, bool one_shot,
                                process_id owner)
{
    if (!valid_uri(uri, uri_size) || !record.moved || !record.resource.valid())
        return EINVAL;

    // Materialize and reserve before taking the registry lock. The lock only
    // protects the name/index commit; URI allocation and vector growth must
    // never run in that critical section.
    freelibcxx::string stored_uri(memory::KernelCommonAllocatorV, uri, static_cast<int>(uri_size));
    if (stored_uri.size() != uri_size)
        return ENOMEM;
    allocation_lock_.lock();
    u64 required_capacity = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        required_capacity = entries_.size() + 1;
    }
    entries_.ensure(required_capacity);
    if (entries_.capacity() < required_capacity)
    {
        allocation_lock_.unlock();
        return ENOMEM;
    }

    khandle object;
    const auto metadata = record.resource.meta();
    i64 result = 0;
    bool inserted = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (find_locked(uri, uri_size) >= 0)
            result = EEXIST;
    }
    if (result != 0)
    {
        allocation_lock_.unlock();
        return result;
    }
    object = record.resource.take_object_to_table();
    if (!object)
    {
        allocation_lock_.unlock();
        return EINVAL;
    }
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (find_locked(uri, uri_size) >= 0)
            result = EEXIST;
        else
        {
            entries_.push_back(std::move(stored_uri), std::move(object), metadata, one_shot, owner);
            inserted = true;
        }
    }
    if (!inserted)
        release_table_root(object);
    allocation_lock_.unlock();
    return result;
}

i64 directory::resolve_service(const char *uri, u64 uri_size, capability::transferred_resource &resource)
{
    if (!valid_uri(uri, uri_size))
        return EINVAL;

    khandle object;
    capability::metadata metadata{};
    bool one_shot = false;
    i64 result = 0;
    allocation_lock_.lock();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto index = find_locked(uri, uri_size);
        if (index < 0)
            result = ENOENT;
        else
        {
            auto &value = entries_[static_cast<u64>(index)];
            if (!value.object)
                result = EIO;
            else
            {
                object = value.object;
                metadata = value.metadata;
                one_shot = value.one_shot || value.object->capability_is_unique();
                if (one_shot)
                {
                    object = std::move(value.object);
                    entries_.remove_at(static_cast<u64>(index));
                }
            }
        }
    }
    allocation_lock_.unlock();
    if (result != 0)
        return result;
    resource = capability::transferred_resource(std::move(object), metadata);
    if (one_shot)
        resource.object()->on_capability_handoff(capability::location::table_root, capability::location::in_transit);
    return 0;
}

i64 directory::listen_service(const char *uri, u64 uri_size, khandle send_endpoint, khandle descriptor, u64 max_pending,
                              process_id owner)
{
    auto release_transferred = [&]() {
        release_table_root(send_endpoint);
        release_table_root(descriptor);
    };
    if (!valid_uri(uri, uri_size) || max_pending == 0 || !send_endpoint || !descriptor)
    {
        release_transferred();
        return EINVAL;
    }

    auto send = send_endpoint.as<naos::ipc::raw_channel_endpoint>();
    auto protocol = descriptor.as<naos::ipc::protocol_descriptor>();
    if (!send || !protocol || send->state() == nullptr || max_pending > send->state()->max_messages())
    {
        release_transferred();
        return EINVAL;
    }

    freelibcxx::string stored_uri(memory::KernelCommonAllocatorV, uri, static_cast<int>(uri_size));
    if (stored_uri.size() != uri_size)
    {
        release_transferred();
        return ENOMEM;
    }
    allocation_lock_.lock();
    u64 required_capacity = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        required_capacity = listeners_.size() + 1;
    }
    listeners_.ensure(required_capacity);
    if (listeners_.capacity() < required_capacity)
    {
        allocation_lock_.unlock();
        release_transferred();
        return ENOMEM;
    }

    listener_entry retired(memory::KernelCommonAllocatorV, khandle{}, khandle{}, 0, 0);
    bool has_retired = false;
    i64 result = 0;

    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (find_locked(uri, uri_size) >= 0)
            result = EEXIST;
        else
        {
            const auto existing = find_listener_locked(uri, uri_size);
            if (existing >= 0)
            {
                auto old_send =
                    listeners_[static_cast<u64>(existing)].send_endpoint.as<naos::ipc::raw_channel_endpoint>();
                auto *old_send_ptr = old_send.operator&();
                if (old_send_ptr != nullptr && old_send_ptr->state() != nullptr &&
                    (old_send_ptr->state()->signals(old_send_ptr->side()) & NA_SIGNAL_PEER_CLOSED) == 0)
                    result = EEXIST;
                else
                {
                    retired = std::move(listeners_[static_cast<u64>(existing)]);
                    listeners_.remove_at(static_cast<u64>(existing));
                    has_retired = true;
                }
            }
            if (result == 0)
                listeners_.push_back(std::move(stored_uri), std::move(send_endpoint), std::move(descriptor), max_pending,
                                             owner);
        }
    }
    allocation_lock_.unlock();
    if (result != 0)
    {
        release_transferred();
        return result;
    }
    if (has_retired)
        release_listener(retired);
    return 0;
}

i64 directory::connect_service(const char *uri, u64 uri_size, const na_uuid_t &expected_uuid, u64 requested_rights,
                               u64 requested_revision, u64 requested_features,
                               capability::transferred_resource &client_resource, u64 &selected_revision,
                               u64 &selected_features)
{
    client_resource = {};
    selected_revision = 0;
    selected_features = 0;
    if (!valid_uri(uri, uri_size))
        return EINVAL;

    khandle listener_endpoint;
    khandle listener_descriptor;
    u64 max_pending = 0;
    listener_entry retired(memory::KernelCommonAllocatorV, khandle{}, khandle{}, 0, 0);
    bool stale_listener = false;
    allocation_lock_.lock();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto index = find_listener_locked(uri, uri_size);
        if (index >= 0)
        {
            auto &listener = listeners_[static_cast<u64>(index)];
            auto send = listener.send_endpoint.as<naos::ipc::raw_channel_endpoint>();
            auto protocol = listener.descriptor.as<naos::ipc::protocol_descriptor>();
            if (!send || !protocol || send->state() == nullptr ||
                (send->state()->signals(send->side()) & NA_SIGNAL_PEER_CLOSED) != 0)
            {
                retired = std::move(listener);
                listeners_.remove_at(static_cast<u64>(index));
                stale_listener = true;
            }
            else
            {
                listener_endpoint = listener.send_endpoint;
                listener_descriptor = listener.descriptor;
                max_pending = listener.max_pending;
            }
        }
    }
    allocation_lock_.unlock();
    if (!listener_endpoint && !stale_listener)
        return ENOENT;
    if (stale_listener)
    {
        release_listener(retired);
        return ENOENT;
    }

    auto send = listener_endpoint.as<naos::ipc::raw_channel_endpoint>();
    auto protocol = listener_descriptor.as<naos::ipc::protocol_descriptor>();
    if (!send || !protocol || send->state() == nullptr)
    {
        return ENOENT;
    }

    const auto &descriptor = protocol->descriptor();
    if (memcmp(descriptor.uuid.bytes, expected_uuid.bytes, sizeof(expected_uuid.bytes)) != 0)
        return EINVAL;
    if (requested_revision != 0 && requested_revision > descriptor.revision)
        return ENOTSUP;
    if ((requested_features & ~descriptor.features) != 0)
        return ENOTSUP;
    const u64 client_protocol_rights = requested_rights == 0 ? descriptor.protocol_rights : requested_rights;
    if ((client_protocol_rights & ~descriptor.protocol_rights) != 0)
        return EACCES;
    khandle client_object;
    khandle server_object;
    capability::metadata client_metadata;
    capability::metadata server_metadata;
    const auto create_status = naos::ipc::create_protocol_endpoint_objects(
        descriptor, nullptr, client_object, server_object, client_metadata, server_metadata);
    if (create_status != NA_STATUS_OK)
    {
        switch (create_status)
        {
            case NA_STATUS_RESOURCE_EXHAUSTED:
                return ENOMEM;
            case NA_STATUS_ACCESS_DENIED:
                return EACCES;
            case NA_STATUS_INVALID_ARGUMENT:
                return EINVAL;
            default:
                return EIO;
        }
    }

    client_metadata.protocol_rights = client_protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    records.push_back(capability::transfer_record(
        NA_HANDLE_INVALID, true, capability::transferred_resource(std::move(server_object), server_metadata)));
    auto *message = memory::New<naos::ipc::channel_message>(memory::KernelCommonAllocatorV, 0, 1);
    if (message == nullptr || !message->valid())
    {
        if (message != nullptr)
            memory::Delete<>(memory::KernelCommonAllocatorV, message);
        return ENOMEM;
    }

    const auto enqueue_status = send->state()->enqueue_kernel(send->side(), message, records, max_pending);
    if (enqueue_status != NA_STATUS_OK)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, message);
        switch (enqueue_status)
        {
            case NA_STATUS_WOULD_BLOCK:
                return EAGAIN;
            case NA_STATUS_RESOURCE_EXHAUSTED:
                return ENOMEM;
            default:
                return EIO;
        }
    }

    client_resource = capability::transferred_resource(std::move(client_object), client_metadata);
    selected_revision = descriptor.revision;
    selected_features = descriptor.features;
    return 0;
}

i64 directory::unregister_service(const char *uri, u64 uri_size, process_id owner)
{
    if (!valid_uri(uri, uri_size))
        return EINVAL;

    entry retired(freelibcxx::string(memory::KernelCommonAllocatorV), khandle{}, {}, false, 0);
    i64 result = 0;
    allocation_lock_.lock();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto index = find_locked(uri, uri_size);
        if (index < 0)
            result = ENOENT;
        else if (owner != 0 && entries_[static_cast<u64>(index)].owner != owner)
            result = EACCES;
        else
        {
            retired = std::move(entries_[static_cast<u64>(index)]);
            entries_.remove_at(static_cast<u64>(index));
        }
    }
    allocation_lock_.unlock();
    if (result != 0)
        return result;
    release_entry(retired);
    return 0;
}

i64 directory::list_services(u64 offset, u64 requested_bytes, freelibcxx::vector<byte> &records, u64 &next,
                             u64 &count) const
{
    if (requested_bytes > NA_CHANNEL_MAX_MESSAGE_BYTES)
        return EINVAL;

    // Reserve output storage before taking the registry lock. The locked
    // section only copies bytes into this buffer and cannot grow it.
    records.ensure(requested_bytes);
    if (records.capacity() < requested_bytes)
        return ENOMEM;

    allocation_lock_.lock();
    i64 result = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (offset > entries_.size())
            result = EINVAL;
        else
        {
            records.clear();
            next = offset;
            count = 0;
            const auto entries = entries_.cspan();
            for (u64 i = offset; i < entries_.size(); i++)
            {
                const auto &uri = entries[i].uri;
                if (uri.size() + 1 > requested_bytes - records.size())
                    break;
                for (u64 j = 0; j < uri.size(); j++)
                    records.push_back(static_cast<byte>(uri.data()[j]));
                records.push_back(static_cast<byte>(0));
                next = i + 1;
                count++;
            }
        }
    }
    allocation_lock_.unlock();
    return result;
}

void directory::cleanup_owner(process_id owner)
{
    if (owner == 0)
        return;

    freelibcxx::vector<khandle> objects(memory::KernelCommonAllocatorV);
    freelibcxx::vector<khandle> endpoints(memory::KernelCommonAllocatorV);
    allocation_lock_.lock();
    u64 entry_count = 0;
    u64 listener_count = 0;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        entry_count = entries_.size();
        listener_count = listeners_.size();
    }
    objects.ensure(entry_count);
    endpoints.ensure(listener_count * 2);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < entries_.size();)
        {
            if (entries_[i].owner != owner)
            {
                i++;
                continue;
            }
            objects.push_back(std::move(entries_[i].object));
            entries_.remove_at(i);
        }
        for (u64 i = 0; i < listeners_.size();)
        {
            if (listeners_[i].owner != owner)
            {
                i++;
                continue;
            }
            endpoints.push_back(std::move(listeners_[i].send_endpoint));
            endpoints.push_back(std::move(listeners_[i].descriptor));
            listeners_.remove_at(i);
        }
    }
    allocation_lock_.unlock();

    for (auto &object : objects)
    {
        release_table_root(object);
    }
    for (auto &endpoint : endpoints)
    {
        release_table_root(endpoint);
    }
}

} // namespace service
