#include "kernel/mm/data_plane.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>
#include <utility>

KLOG_MODULE(mm);

namespace naos::syscall
{
namespace
{
template <typename T> na_status_t copy_in(const T *source, T &destination)
{
    return usercopy::copy_versioned(destination, source);
}
} // namespace

na_status_t memory_map(na_memory_map_frame_t *frame)
{
    na_memory_map_frame_t values{};
    auto status = copy_in(frame, values);
    if (status != NA_STATUS_OK)
        return status;
    if (values.struct_size < sizeof(values) ||
        values.flags & ~(NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_EXEC | NA_MEMORY_MAP_SHARED) ||
        (values.object == NA_HANDLE_INVALID && values.offset != 0) || values.length == 0 || values.address != 0 ||
        values.data_offset != 0 || values.reserved0 != 0 || values.reserved1 != 0 ||
        (values.hint != 0 && (!is_user_space_pointer(values.hint) || (values.hint & (memory::page_size - 1)) != 0)) ||
        values.length > NA_MEMORY_MAP_MAX_BYTES)
        return NA_STATUS_INVALID_ARGUMENT;
    naos::data_plane::memory_object *memory_object = nullptr;
    khandle backing;
    capability::metadata object_meta;
    if (values.object != NA_HANDLE_INVALID)
    {
        capability::entry entry;
        if (!task::current_process()->resource.lookup_native(values.object, entry) || !entry.object)
            return NA_STATUS_INVALID_HANDLE;
        object_meta = entry.meta;
        if (entry.meta.binding == NA_BINDING_MEMORY_OBJECT && entry.meta.scope == NA_SCOPE_MEMORY_OBJECT)
        {
            if ((entry.meta.protocol_rights & NA_MEMORY_RIGHT_MAP) == 0)
                return NA_STATUS_ACCESS_DENIED;
            if ((values.flags & NA_MEMORY_MAP_READ) != 0 && (entry.meta.protocol_rights & NA_MEMORY_RIGHT_READ) == 0)
                return NA_STATUS_ACCESS_DENIED;
            if ((values.flags & NA_MEMORY_MAP_WRITE) != 0 && (entry.meta.protocol_rights & NA_MEMORY_RIGHT_WRITE) == 0)
                return NA_STATUS_ACCESS_DENIED;
            memory_object = entry.object->get<naos::data_plane::memory_object>();
            if (memory_object == nullptr || values.offset > entry.meta.view_length ||
                values.length > entry.meta.view_length - values.offset)
                return NA_STATUS_INVALID_ARGUMENT;
            backing = entry.object;
        }
        else
            return NA_STATUS_WRONG_BINDING;
    }
    flag_t vm_flags = 0;
    if ((values.flags & NA_MEMORY_MAP_READ) != 0)
        vm_flags |= memory::vm::flags::readable;
    if ((values.flags & NA_MEMORY_MAP_WRITE) != 0)
        vm_flags |= memory::vm::flags::writeable;
    if ((values.flags & NA_MEMORY_MAP_EXEC) != 0)
        vm_flags |= memory::vm::flags::executeable;
    if ((values.flags & NA_MEMORY_MAP_SHARED) != 0)
        vm_flags |= memory::vm::flags::shared;
    auto *vm_info = reinterpret_cast<memory::vm::info_t *>(task::current_process()->mm_info);
    u64 object_offset = values.offset;
    u64 data_offset = 0;
    u64 map_length = values.length;
    if (memory_object != nullptr)
    {
        if (object_meta.view_offset > memory_object->size() ||
            object_offset > memory_object->size() - object_meta.view_offset)
            return NA_STATUS_INVALID_ARGUMENT;
        if (object_meta.view_offset > std::numeric_limits<u64>::max() - object_offset)
            return NA_STATUS_INVALID_ARGUMENT;
        object_offset += object_meta.view_offset;
        const u64 aligned_offset = object_offset & ~(memory::page_size - 1);
        const u64 delta = object_offset - aligned_offset;
        if (values.length > std::numeric_limits<u64>::max() - delta)
            return NA_STATUS_INVALID_ARGUMENT;
        map_length = values.length + delta;
        data_offset = delta;
        object_offset = aligned_offset;
    }
    const auto *vm =
        memory_object != nullptr
            ? vm_info->map_memory_object(values.hint, std::move(backing), memory_object, object_offset, data_offset,
                                         values.length, map_length, vm_flags)
            : vm_info->vma().allocate_map((values.length + memory::page_size - 1) & ~(memory::page_size - 1),
                                          vm_flags | memory::vm::flags::lock | memory::vm::flags::user_mode |
                                              memory::vm::flags::expand,
                                          memory::vm::page_fault_method::common, 0);
    if (vm == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    values.address = vm->start;
    values.data_offset = data_offset;
    status = usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
    if (status != NA_STATUS_OK)
    {
        vm_info->unmap(vm->start, vm->end - vm->start);
        return status;
    }
    return NA_STATUS_OK;
}

na_status_t memory_unmap(na_memory_unmap_frame_t *frame)
{
    na_memory_unmap_frame_t values{};
    auto status = copy_in(frame, values);
    if (status != NA_STATUS_OK)
        return status;
    if (values.struct_size < sizeof(values) || values.flags != 0 || values.address == 0 || values.length == 0 ||
        values.reserved0 != 0 || values.reserved1 != 0 || (values.address & (memory::page_size - 1)) != 0 ||
        values.length > NA_MEMORY_MAP_MAX_BYTES)
        return NA_STATUS_INVALID_ARGUMENT;
    const auto rounded = (values.length + memory::page_size - 1) & ~(memory::page_size - 1);
    if (rounded < values.length)
        return NA_STATUS_INVALID_ARGUMENT;
    auto *vm_info = reinterpret_cast<memory::vm::info_t *>(task::current_process()->mm_info);
    const bool unmapped = vm_info->unmap(values.address, rounded);
    if (!unmapped)
        KLOG_WARN("memory unmap failed pid {} address {} length {}", task::current_process()->pid,
                  log::hex(values.address), log::hex(rounded));
    return unmapped ? NA_STATUS_OK : NA_STATUS_INVALID_ARGUMENT;
}

na_status_t memory_create(u64 size, u64 flags, na_handle_t *result)
{
    if (result == nullptr || !is_user_space_range(result, sizeof(*result)))
        return NA_STATUS_FAULT;

    if ((flags & ~(u64)NA_MEMORY_FLAG_READ_ONLY) != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    // Oversized requests are the EFBIG-equivalent negative path
    // (USERSPACE_FILESYSTEM_ADR §5.3.2); the global cap is never raised here.
    if (size == 0 || size > NA_MEMORY_OBJECT_MAX_BYTES)
        return NA_STATUS_INVALID_ARGUMENT;

    auto object = handle_t<data_plane::memory_object>::make(size, static_cast<u32>(flags));
    if (!object || object->size() != size)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    capability::metadata metadata;
    metadata.binding = NA_BINDING_MEMORY_OBJECT;
    metadata.protocol_uuid = naos::system::MemoryObject::protocol_uuid;
    metadata.scope = NA_SCOPE_MEMORY_OBJECT;
    metadata.revision = naos::system::MemoryObject::revision;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    metadata.view_offset = 0;
    metadata.view_length = size;
    // Creators get read/map/info plus write unless the object is read-only.
    // INVOKE admits the object into the kernel dispatch path so callers can
    // fill/read buffers via MemoryObject.read/write invocations.
    metadata.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE | NA_MEMORY_RIGHT_READ | NA_MEMORY_RIGHT_MAP |
                               NA_MEMORY_RIGHT_INFO |
                               ((flags & NA_MEMORY_FLAG_READ_ONLY) != 0 ? 0 : NA_MEMORY_RIGHT_WRITE);

    auto &resources = task::current_process()->resource;
    const auto handle_out = resources.install_native(std::move(object), metadata);
    if (handle_out == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;

    const auto out_status = usercopy::copy_to(reinterpret_cast<u64>(result), &handle_out, sizeof(handle_out));
    if (out_status != NA_STATUS_OK)
    {
        resources.close_native(handle_out);
        return out_status;
    }
    return NA_STATUS_OK;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_MEMORY_MAP, memory_map)
SYSCALL(NA_SYSCALL_MEMORY_UNMAP, memory_unmap)
SYSCALL(NA_SYSCALL_MEMORY_CREATE, memory_create)
END_SYSCALL
} // namespace naos::syscall
