#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/msg_queue.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

namespace naos::syscall
{
u64 brk(u64 ptr)
{
    if (!is_user_space_pointer(ptr))
    {
        return EPARAM;
    }
    auto info = (memory::vm::info_t *)(task::current_process()->mm_info);
    if (info->set_brk(ptr))
    {
        return OK;
    }
    return EFAILED;
}

u64 sbrk(i64 offset)
{
    auto info = (memory::vm::info_t *)(task::current_process()->mm_info);
    if (offset == 0)
    {
        return info->get_brk();
    }
    auto r = info->get_brk();
    info->set_brk(r + offset);
    return r;
}

/// map memory/file
///
/// \param flags 1:read;2:write;4:exec;8:file;16:share
u64 map(u64 map_address, file_desc fd, u64 offset, u64 length, flag_t flags)
{
    if (!is_user_space_pointer(map_address) || !is_user_space_pointer(map_address + length))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    auto vm_info = ((memory::vm::info_t *)(task::current_process()->mm_info));
    fs::vfs::file *file = nullptr;
    if (flags & 8)
    {
        auto obj = res.get_kobject(fd);
        if (obj)
            return ENOEXIST;
    }
    auto vm = vm_info->map_file(map_address, file, offset, length, length, flags);

    if (vm)
        return vm->start;
    return EFAILED;
}

u64 umap(u64 address, u64 size)
{
    if (!is_user_space_pointer(address))
    {
        return EPARAM;
    }
    auto vm_info = ((memory::vm::info_t *)(task::current_process()->mm_info));
    if (vm_info->umap_file(address, size))
        return OK;
    return EFAILED;
}

unsigned long create_msg_queue(unsigned long msg_count, unsigned long msg_bytes)
{
    auto q = memory::create_msg_queue(msg_count, msg_bytes);
    if (q == nullptr)
        return EPARAM;
    return q->key;
}

unsigned long write_msg_queue(unsigned long key, unsigned long type, void *buffer, unsigned long size,
                              unsigned long flags)
{
    auto q = memory::get_msg_queue(key);
    if (q == nullptr)
        return EPARAM;
    if (!is_user_space_pointer(buffer) || !is_user_space_pointer((byte *)buffer + size))
        return EBUFFER;
    return memory::write_msg(q, type, (byte *)buffer, size, flags);
}

unsigned long read_msg_queue(unsigned long key, unsigned long type, void *buffer, unsigned long size,
                             unsigned long flags)
{
    auto q = memory::get_msg_queue(key);
    if (q == nullptr)
        return EPARAM;
    if (!is_user_space_pointer(buffer) || !is_user_space_pointer((byte *)buffer + size))
        return EBUFFER;
    auto ret = memory::read_msg(q, type, (byte *)buffer, size, flags);
    if (ret == -1)
        return EOF;
    if (ret == -2)
        return ECONTI;
    return ret;
}

unsigned long close_msg_queue(unsigned long key)
{
    auto q = memory::get_msg_queue(key);
    if (q == nullptr)
        return EPARAM;
    if (memory::close_msg_queue(q))
        return OK;
    return ERESOURCE_NOT_NULL;
}

BEGIN_SYSCALL

SYSCALL(57, brk)
SYSCALL(58, sbrk)
SYSCALL(59, map)
SYSCALL(60, umap)
SYSCALL(61, create_msg_queue)
SYSCALL(62, write_msg_queue)
SYSCALL(63, read_msg_queue)
SYSCALL(64, close_msg_queue)

END_SYSCALL
} // namespace syscall
