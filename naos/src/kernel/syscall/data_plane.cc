#include "kernel/mm/data_plane.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system_uapi.h"
#include <utility>

namespace naos::syscall
{
namespace
{
template <typename T> na_status_t copy_in(const T *source, T &destination)
{
    return usercopy::copy_versioned(destination, source);
}
} // namespace

u64 memory_map(na_memory_map_frame_t *frame)
{
    na_memory_map_frame_t values{};
    auto status = copy_in(frame, values);
    if (status != NA_STATUS_OK)
        return status;
    if (values.struct_size < sizeof(values) ||
        values.flags & ~(NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_EXEC | NA_MEMORY_MAP_SHARED) ||
        (values.object == NA_HANDLE_INVALID && values.offset != 0) || values.length == 0 || values.address != 0 ||
        values.reserved0 != 0 || values.reserved1 != 0 || (values.offset & (memory::page_size - 1)) != 0 ||
        (values.hint != 0 && (!is_user_space_pointer(values.hint) || (values.hint & (memory::page_size - 1)) != 0)) ||
        values.length > NA_MEMORY_MAP_MAX_BYTES)
        return NA_STATUS_INVALID_ARGUMENT;
    if ((values.flags & NA_MEMORY_MAP_SHARED) != 0)
        return NA_STATUS_NOT_SUPPORTED;

    fs::vfs::file *file = nullptr;
    naos::data_plane::memory_object *memory_object = nullptr;
    khandle backing;
    if (values.object != NA_HANDLE_INVALID)
    {
        capability::entry entry;
        if (!task::current_process()->resource.lookup_native(values.object, entry) || !entry.object)
            return NA_STATUS_INVALID_HANDLE;
        if (entry.meta.binding == NA_BINDING_MEMORY_OBJECT && entry.meta.scope == NA_SCOPE_MEMORY_OBJECT)
        {
            if ((entry.meta.protocol_rights & NA_MEMORY_RIGHT_MAP) == 0)
                return NA_STATUS_ACCESS_DENIED;
            if ((values.flags & NA_MEMORY_MAP_READ) != 0 && (entry.meta.protocol_rights & NA_MEMORY_RIGHT_READ) == 0)
                return NA_STATUS_ACCESS_DENIED;
            if ((values.flags & NA_MEMORY_MAP_WRITE) != 0 && (entry.meta.protocol_rights & NA_MEMORY_RIGHT_WRITE) == 0)
                return NA_STATUS_ACCESS_DENIED;
            memory_object = entry.object->get<naos::data_plane::memory_object>();
            if (memory_object == nullptr || values.offset > memory_object->size() ||
                values.length > memory_object->size() - values.offset)
                return NA_STATUS_INVALID_ARGUMENT;
            backing = entry.object;
        }
        else
        {
            if (entry.meta.scope != NA_SCOPE_FILE && entry.meta.scope != NA_SCOPE_STREAM)
                return NA_STATUS_WRONG_SCOPE;
            file = entry.object->get<fs::vfs::file>();
            if (file == nullptr)
                return NA_STATUS_WRONG_BINDING;
        }
    }
    flag_t vm_flags = 0;
    if ((values.flags & NA_MEMORY_MAP_READ) != 0)
        vm_flags |= memory::vm::flags::readable;
    if ((values.flags & NA_MEMORY_MAP_WRITE) != 0)
        vm_flags |= memory::vm::flags::writeable;
    if ((values.flags & NA_MEMORY_MAP_EXEC) != 0)
        vm_flags |= memory::vm::flags::executeable;
    auto *vm_info = reinterpret_cast<memory::vm::info_t *>(task::current_process()->mm_info);
    const auto *vm = memory_object != nullptr
                         ? vm_info->map_memory_object(values.hint, std::move(backing), memory_object, values.offset,
                                                      values.length, vm_flags)
                         : vm_info->map_file(values.hint, file, values.offset, values.length, values.length, vm_flags);
    if (vm == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    values.address = vm->start;
    status = usercopy::copy_to(reinterpret_cast<u64>(frame), &values, sizeof(values));
    if (status != NA_STATUS_OK)
    {
        vm_info->umap_file(vm->start, vm->end - vm->start);
        return status;
    }
    return NA_STATUS_OK;
}

u64 memory_unmap(na_memory_unmap_frame_t *frame)
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
    return vm_info->umap_file(values.address, rounded) ? NA_STATUS_OK : NA_STATUS_INVALID_ARGUMENT;
}

BEGIN_SYSCALL
SYSCALL(NA_SYSCALL_MEMORY_MAP, memory_map)
SYSCALL(NA_SYSCALL_MEMORY_UNMAP, memory_unmap)
END_SYSCALL
} // namespace naos::syscall
