#include "kernel/fs/vfs/file.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"
namespace syscall
{
file_desc open(const char *filepath, u64 mode, u64 flags)
{
    if (filepath == nullptr || !is_user_space_pointer(filepath))
    {
        return EPARAM;
    }

    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();

    auto file = fs::vfs::open(filepath, ft->root, ft->current, mode, flags);
    if (file)
    {
        auto fd = res.new_file_desc(file);
        return fd;
    }
    return ENOEXIST;
}

u64 close(file_desc fd)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        res.delete_file_desc(fd);
        file->close();
        return OK;
    }
    return ENOEXIST;
}

i64 write(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    if (buffer == nullptr || !is_user_space_pointer(buffer) || !is_user_space_pointer(buffer + max_len))
    {
        return EBUFFER;
    }
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        return file->write(buffer, max_len, flags);
    }
    return ENOEXIST;
}

i64 read(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    if (buffer == nullptr || !is_user_space_pointer(buffer) || !is_user_space_pointer(buffer + max_len))
    {
        return EBUFFER;
    }

    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        return file->read(buffer, max_len, flags);
    }
    return ENOEXIST;
}

i64 pwrite(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        file->move(offset);
        return file->write(buffer, max_len, flags);
    }
    return ENOEXIST;
}

i64 pread(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        file->move(offset);
        return file->read(buffer, max_len, flags);
    }
    return ENOEXIST;
}

/// seek file offset
///
/// \param fd the file desc
/// \param offset the offset
/// \param type 0: from current 1: offset of file begin 2: offset of file end
/// \return old offset
i64 lseek(file_desc fd, i64 offset, u64 type)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        auto old = file->current_offset();
        if (type == 0)
            file->seek(offset);
        else if (type == 1)
            file->move(offset);
        else if (type == 2)
        {
            file->move(file->size() - offset);
        }
        return old;
    }
    return 0;
}

i64 select(u64 fd_count, file_desc *in, file_desc *out, file_desc *err, u64 flags) {}

i64 get_pipe(file_desc *fd1, file_desc *fd2)
{
    auto file = fs::vfs::open_pipe();
    if (!file)
        return EFAILED;
    auto &res = task::current_process()->res_table;
    auto fdx0 = res.new_file_desc(file);
    if (fdx0 == invalid_file_desc)
    {
        file->close();
        return EFAILED;
    }

    auto fdx1 = res.new_file_desc(file);
    if (fdx1 == invalid_file_desc)
    {
        file->close();
        res.delete_file_desc(fdx0);
        return EFAILED;
    }
    file->add_ref();

    *fd1 = fdx0;
    *fd2 = fdx1;

    return OK;
}

file_desc create_fifo(const char *path, u64 mode)
{
    if (path == nullptr || !is_user_space_pointer(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    auto fd = ft->id_gen.next();
    if (fd == util::null_id)
    {
        return EFAILED;
    }

    auto file = fs::vfs::create_fifo(path, ft->root, ft->current, mode);
    if (!file)
    {
        ft->id_gen.collect(fd);
        return EFAILED;
    }
    res.set_file(fd, file);

    return fd;
}

i64 fcntl(file_desc fd, u64 operator_type, u64 target, u64 attr, u64 *value, u64 size)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (!file)
    {
        return EFAILED;
    }
    if (!fs::vfs::fcntl(file, operator_type, target, attr, value, size))
    {
        return EFAILED;
    }
    return OK;
}

BEGIN_SYSCALL
SYSCALL(2, open)
SYSCALL(3, close)
SYSCALL(4, write)
SYSCALL(5, read)
SYSCALL(6, pwrite)
SYSCALL(7, pread)
SYSCALL(8, lseek)
SYSCALL(9, select)
SYSCALL(10, get_pipe)
SYSCALL(11, create_fifo)
SYSCALL(12, fcntl)
END_SYSCALL
} // namespace syscall
