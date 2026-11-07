#pragma once

#include "invocation_support.hpp"
#include "kernel/mm/data_plane.hpp"

namespace naos::ipc::dispatch_detail
{
inline naos::data_plane::memory_object *request_region_object(capability::transfer_record_list &resources, u64 index,
                                                              u64 required_rights, u64 view_offset, u64 view_length,
                                                              u64 &absolute_offset)
{
    if (index >= resources.size())
        return nullptr;
    auto &record = resources[index];
    if (record.moved || !record.resource.valid())
        return nullptr;
    const auto &meta = record.resource.meta();
    if (meta.binding != NA_BINDING_MEMORY_OBJECT || meta.scope != NA_SCOPE_MEMORY_OBJECT ||
        (meta.meta_rights & NA_RIGHT_TRANSFER) == 0 || (meta.protocol_rights & required_rights) != required_rights ||
        view_length == 0 || view_offset > meta.view_length || view_length > meta.view_length - view_offset ||
        meta.view_offset > ~u64(0) - view_offset)
        return nullptr;
    absolute_offset = meta.view_offset + view_offset;
    return record.resource.object()->get<naos::data_plane::memory_object>();
}
} // namespace naos::ipc::dispatch_detail
