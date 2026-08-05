#include "kernel/resource.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"
#include <utility>
namespace task
{
resource_table_t::resource_table_t()
    : native_handle_map(memory::KernelCommonAllocatorV)
    , next_native_handle(1)
    , native_entry_count(0)
{
}

resource_table_t::~resource_table_t() { clear(); }

na_handle_t resource_table_t::allocate_native_handle_locked()
{
    if (native_entry_count >= NA_CAPABILITY_MAX_PER_PROCESS || next_native_handle == NA_HANDLE_INVALID)
        return NA_HANDLE_INVALID;

    const na_handle_t handle = next_native_handle;
    next_native_handle++;
    return handle;
}

void resource_table_t::clear_native_locked()
{
    for (auto it = native_handle_map.begin(); it != native_handle_map.end(); ++it)
    {
        if (it->value.object)
            it->value.object->on_capability_release(capability::location::table_root);
    }
    native_handle_map.clear();
    native_entry_count = 0;
}

na_handle_t resource_table_t::install_native(khandle object, capability::metadata meta)
{
    if (!object)
        return NA_HANDLE_INVALID;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    const na_handle_t handle = allocate_native_handle_locked();
    if (handle == NA_HANDLE_INVALID)
        return NA_HANDLE_INVALID;

    capability::entry entry;
    entry.object = std::move(object);
    entry.generation = handle;
    entry.state = capability::entry_state::active;
    entry.meta = meta;
    native_handle_map.insert(handle, std::move(entry));
    native_entry_count++;
    native_handle_map.get(handle).value().object->on_capability_acquire(capability::location::table_root);
    return handle;
}

na_status_t resource_table_t::reserve_native(freelibcxx::vector<na_handle_t> &handles, u64 count)
{
    handles.clear();
    if (count > NA_CAPABILITY_MAX_PER_PROCESS)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    handles.ensure(count);
    if (count != 0 && handles.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    for (u64 i = 0; i < count; i++)
    {
        const na_handle_t handle = allocate_native_handle_locked();
        if (handle == NA_HANDLE_INVALID)
        {
            for (auto reserved : handles)
                native_handle_map.remove(reserved);
            native_entry_count -= handles.size();
            handles.clear();
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }

        capability::entry entry;
        entry.generation = handle;
        entry.state = capability::entry_state::reserved;
        native_handle_map.insert(handle, std::move(entry));
        native_entry_count++;
        handles.push_back(handle);
    }
    return NA_STATUS_OK;
}

na_status_t resource_table_t::activate_native(na_handle_t handle, capability::transferred_resource &&resource)
{
    if (handle == NA_HANDLE_INVALID || !resource.valid())
        return NA_STATUS_INVALID_ARGUMENT;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto *existing = native_handle_map.get_ptr(handle);
    if (existing == nullptr || existing->state != capability::entry_state::reserved)
        return NA_STATUS_INVALID_HANDLE;

    auto object = resource.take_object_to_table();
    if (!object)
        return NA_STATUS_INVALID_ARGUMENT;
    existing->object = std::move(object);
    existing->state = capability::entry_state::active;
    existing->meta = resource.meta();
    return NA_STATUS_OK;
}

void resource_table_t::rollback_native(const freelibcxx::vector<na_handle_t> &handles)
{
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    for (auto handle : handles)
    {
        auto existing = native_handle_map.get(handle);
        if (existing.has_value() && existing->state == capability::entry_state::reserved)
        {
            native_handle_map.remove(handle);
            native_entry_count--;
        }
    }
}

bool resource_table_t::lookup_native(na_handle_t handle, capability::entry &entry)
{
    uctx::RawReadLockUninterruptibleContext icu(native_map_lock);
    auto found = native_handle_map.get(handle);
    if (!found.has_value() || found->state != capability::entry_state::active)
        return false;
    entry = std::move(found.value());
    return true;
}

na_signal_t resource_table_t::native_signals(na_handle_t handle)
{
    capability::entry entry;
    if (!lookup_native(handle, entry) || !entry.object)
        return 0;
    return entry.object->capability_signals();
}

na_status_t resource_table_t::close_native(na_handle_t handle)
{
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_INVALID_HANDLE;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto found = native_handle_map.get(handle);
    if (!found.has_value() || found->state != capability::entry_state::active)
        return NA_STATUS_INVALID_HANDLE;
    if (found->object)
        found->object->on_capability_release(capability::location::table_root);
    native_handle_map.remove(handle);
    native_entry_count--;
    return NA_STATUS_OK;
}

na_status_t resource_table_t::duplicate_native(na_handle_t source, na_meta_rights_t requested_rights,
                                               na_handle_t &result)
{
    result = NA_HANDLE_INVALID;
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto found = native_handle_map.get(source);
    if (!found.has_value() || found->state != capability::entry_state::active)
        return NA_STATUS_INVALID_HANDLE;
    if (!found->object || found->object->capability_is_unique())
        return NA_STATUS_ACCESS_DENIED;
    if ((found->meta.meta_rights & NA_RIGHT_DUPLICATE) == 0)
        return NA_STATUS_ACCESS_DENIED;

    const auto rights = requested_rights == 0 ? found->meta.meta_rights : requested_rights;
    if ((rights & ~found->meta.meta_rights) != 0)
        return NA_STATUS_ACCESS_DENIED;

    const na_handle_t handle = allocate_native_handle_locked();
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    // `get()` returns a snapshot. Keep the source entry in the table: a
    // duplicate creates a second capability with attenuated rights, while a
    // move is expressed explicitly through a MOVE disposition.
    auto duplicated = found.value();
    duplicated.meta.meta_rights = rights;
    duplicated.generation = handle;
    native_handle_map.insert(handle, std::move(duplicated));
    native_entry_count++;
    native_handle_map.get(handle).value().object->on_capability_acquire(capability::location::table_root);
    result = handle;
    return NA_STATUS_OK;
}

na_status_t resource_table_t::restrict_native(na_handle_t source, const na_handle_restriction_t &restriction,
                                              na_handle_t &result, capability::entry &source_backup)
{
    result = NA_HANDLE_INVALID;
    source_backup = {};
    constexpr u32 known_flags = NA_RESTRICTION_SCOPE | NA_RESTRICTION_REVISION | NA_RESTRICTION_FEATURES |
                                NA_RESTRICTION_META_RIGHTS | NA_RESTRICTION_PROTOCOL_RIGHTS;
    if (restriction.struct_size < sizeof(restriction) || (restriction.flags & ~known_flags) != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto found = native_handle_map.get(source);
    if (!found.has_value() || found->state != capability::entry_state::active)
        return NA_STATUS_INVALID_HANDLE;
    const auto rights =
        (restriction.flags & NA_RESTRICTION_META_RIGHTS) == 0 ? found->meta.meta_rights : restriction.meta_rights;
    if ((rights & ~found->meta.meta_rights) != 0)
        return NA_STATUS_ACCESS_DENIED;
    const auto protocol_rights = (restriction.flags & NA_RESTRICTION_PROTOCOL_RIGHTS) == 0
                                     ? found->meta.protocol_rights
                                     : restriction.protocol_rights;
    if ((protocol_rights & ~found->meta.protocol_rights) != 0)
        return NA_STATUS_ACCESS_DENIED;
    if ((restriction.flags & NA_RESTRICTION_SCOPE) != 0 &&
        (restriction.scope == 0 || restriction.scope != found->meta.scope))
        return NA_STATUS_ACCESS_DENIED;
    if ((restriction.flags & NA_RESTRICTION_REVISION) != 0 &&
        (restriction.revision == 0 || restriction.revision > found->meta.revision))
        return NA_STATUS_ACCESS_DENIED;
    if ((restriction.flags & NA_RESTRICTION_FEATURES) != 0 && (restriction.features & ~found->meta.features) != 0)
        return NA_STATUS_ACCESS_DENIED;
    auto *source_entry = native_handle_map.get_ptr(source);
    if (source_entry == nullptr)
        return NA_STATUS_INVALID_HANDLE;

    const na_handle_t handle = allocate_native_handle_locked();
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    source_backup = found.value();
    auto restricted = source_backup;
    restricted.state = capability::entry_state::reserved;
    restricted.object.reset();
    restricted.meta.meta_rights = rights;
    restricted.meta.protocol_rights = protocol_rights;
    if ((restriction.flags & NA_RESTRICTION_SCOPE) != 0)
        restricted.meta.scope = restriction.scope;
    if ((restriction.flags & NA_RESTRICTION_REVISION) != 0)
        restricted.meta.revision = restriction.revision;
    if ((restriction.flags & NA_RESTRICTION_FEATURES) != 0)
        restricted.meta.features = restriction.features;
    restricted.generation = handle;
    source_entry->state = capability::entry_state::restricting;
    native_handle_map.insert(handle, std::move(restricted));
    result = handle;
    return NA_STATUS_OK;
}

na_status_t resource_table_t::commit_restrict(na_handle_t source, na_handle_t restricted)
{
    if (source == NA_HANDLE_INVALID || restricted == NA_HANDLE_INVALID)
        return NA_STATUS_INVALID_ARGUMENT;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto *source_entry = native_handle_map.get_ptr(source);
    auto *pending = native_handle_map.get_ptr(restricted);
    if (source_entry == nullptr || source_entry->state != capability::entry_state::restricting ||
        !source_entry->object || pending == nullptr || pending->state != capability::entry_state::reserved)
        return NA_STATUS_INVALID_HANDLE;

    auto source_object = std::move(source_entry->object);
    pending->object = std::move(source_object);
    pending->state = capability::entry_state::active;
    pending->object->on_capability_acquire(capability::location::table_root);
    pending->object->on_capability_release(capability::location::table_root);
    native_handle_map.remove(source);
    native_entry_count--;
    return NA_STATUS_OK;
}

na_status_t resource_table_t::rollback_restrict(na_handle_t source, na_handle_t restricted,
                                                capability::entry &source_backup)
{
    if (source == NA_HANDLE_INVALID || restricted == NA_HANDLE_INVALID || !source_backup.object)
        return NA_STATUS_INVALID_ARGUMENT;
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    auto source_entry = native_handle_map.get_ptr(source);
    auto restricted_entry = native_handle_map.get_ptr(restricted);
    if (source_entry == nullptr || source_entry->state != capability::entry_state::restricting ||
        !source_entry->object || restricted_entry == nullptr ||
        restricted_entry->state != capability::entry_state::reserved)
        return NA_STATUS_INVALID_HANDLE;
    native_handle_map.remove(restricted);
    native_entry_count--;
    source_entry->state = capability::entry_state::active;
    return NA_STATUS_OK;
}

na_status_t resource_table_t::clone_fork_bindings(const resource_table_t &source)
{
    if (this == &source)
        return NA_STATUS_INVALID_ARGUMENT;

    uctx::RawReadLockUninterruptibleContext source_guard(const_cast<lock::rw_lock_t &>(source.native_map_lock));
    uctx::RawWriteLockUninterruptibleContext destination_guard(native_map_lock);
    if (native_entry_count != 0)
        return NA_STATUS_INVALID_ARGUMENT;

    u64 clonable = 0;
    for (auto it = source.native_handle_map.begin(); it != source.native_handle_map.end(); ++it)
    {
        const auto &entry = it->value;
        if (entry.state == capability::entry_state::active && entry.object &&
            entry.meta.binding == NA_BINDING_KERNEL_VIEW && !entry.object->capability_is_unique() &&
            (entry.meta.meta_rights & NA_RIGHT_DUPLICATE) != 0)
            clonable++;
    }
    if (clonable > NA_CAPABILITY_MAX_PER_PROCESS)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    next_native_handle = source.next_native_handle;
    for (auto it = source.native_handle_map.begin(); it != source.native_handle_map.end(); ++it)
    {
        const auto &entry = it->value;
        if (entry.state != capability::entry_state::active || !entry.object ||
            entry.meta.binding != NA_BINDING_KERNEL_VIEW || entry.object->capability_is_unique() ||
            (entry.meta.meta_rights & NA_RIGHT_DUPLICATE) == 0)
            continue;

        capability::entry cloned = entry;
        cloned.generation = it->key;
        native_handle_map.insert(it->key, std::move(cloned));
        native_entry_count++;
        native_handle_map.get(it->key).value().object->on_capability_acquire(capability::location::table_root);
    }
    return NA_STATUS_OK;
}

na_status_t resource_table_t::take_native_batch(const na_resource_disposition_t *dispositions, u64 count,
                                                na_handle_t target, capability::transfer_record_list &records)
{
    records.clear();
    if (count > NA_CHANNEL_MAX_RESOURCES || (count != 0 && dispositions == nullptr))
        return NA_STATUS_INVALID_ARGUMENT;
    records.ensure(count);
    if (count != 0 && records.data() == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    freelibcxx::vector<capability::entry> snapshots(memory::KernelCommonAllocatorV);
    freelibcxx::vector<u32> operations(memory::KernelCommonAllocatorV);
    snapshots.ensure(count);
    operations.ensure(count);
    if ((count != 0 && snapshots.data() == nullptr) || (count != 0 && operations.data() == nullptr))
        return NA_STATUS_RESOURCE_EXHAUSTED;

    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    for (u64 i = 0; i < count; i++)
    {
        const auto &disposition = dispositions[i];
        if (disposition.handle == NA_HANDLE_INVALID || disposition.flags != 0 || disposition.handle == target)
            return NA_STATUS_INVALID_ARGUMENT;
        for (u64 j = 0; j < i; j++)
        {
            if (dispositions[j].handle == disposition.handle)
                return NA_STATUS_INVALID_ARGUMENT;
        }

        auto found = native_handle_map.get(disposition.handle);
        if (!found.has_value() || found->state != capability::entry_state::active)
            return NA_STATUS_INVALID_HANDLE;
        if (!found->object)
            return NA_STATUS_INVALID_HANDLE;

        const auto operation = disposition.operation;
        if (operation != NA_RESOURCE_MOVE && operation != NA_RESOURCE_DUPLICATE)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((found->meta.meta_rights & NA_RIGHT_TRANSFER) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (operation == NA_RESOURCE_DUPLICATE &&
            ((found->meta.meta_rights & NA_RIGHT_DUPLICATE) == 0 || found->object->capability_is_unique()))
            return NA_STATUS_ACCESS_DENIED;

        const auto rights = disposition.rights == 0 ? found->meta.meta_rights : disposition.rights;
        if ((rights & ~found->meta.meta_rights) != 0)
            return NA_STATUS_ACCESS_DENIED;
        if (disposition.scope != 0 && disposition.scope != found->meta.scope)
            return NA_STATUS_ACCESS_DENIED;

        found->meta.meta_rights = rights;
        if (disposition.scope != 0)
            found->meta.scope = disposition.scope;
        snapshots.push_back(std::move(found.value()));
        operations.push_back(operation);
    }

    for (u64 i = 0; i < count; i++)
    {
        auto &snapshot = snapshots[i];
        const bool moved = operations[i] == NA_RESOURCE_MOVE;
        capability::transferred_resource resource(snapshot.object, snapshot.meta);
        if (moved)
        {
            native_handle_map.remove(dispositions[i].handle);
            native_entry_count--;
            snapshot.object->on_capability_handoff(capability::location::table_root, capability::location::in_transit);
        }
        records.push_back(capability::transfer_record(dispositions[i].handle, moved, std::move(resource)));
    }
    return NA_STATUS_OK;
}

na_status_t resource_table_t::restore_native_batch(capability::transfer_record_list &records)
{
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    u64 moved_count = 0;
    for (auto &record : records)
    {
        if (record.moved)
        {
            moved_count++;
            if (record.resource.valid() && native_handle_map.has(record.source))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    if (native_entry_count + moved_count > NA_CAPABILITY_MAX_PER_PROCESS)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    for (auto &record : records)
    {
        if (!record.moved)
        {
            record.resource.release_transit();
            continue;
        }
        if (!record.resource.valid())
            continue;
        auto object = record.resource.take_object_to_table();
        capability::entry entry;
        entry.object = std::move(object);
        entry.generation = record.source;
        entry.state = capability::entry_state::active;
        entry.meta = record.resource.meta();
        native_handle_map.insert(record.source, std::move(entry));
        native_entry_count++;
    }
    return NA_STATUS_OK;
}

void resource_table_t::clear()
{
    uctx::RawWriteLockUninterruptibleContext icu(native_map_lock);
    clear_native_locked();
}

} // namespace task
