#include "kernel/fs/vfs/file.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"
namespace naos::syscall
{
file_desc open(const char *path, u64 mode, u64 flags)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }

    auto &res = task::current_process()->resource;
    auto file = fs::vfs::open(path, res.root(), res.current(), mode, flags);
    if (file)
    {
        auto fd = res.new_kobject(file);
        return fd;
    }
    return ENOEXIST;
}

u64 close(file_desc fd)
{
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        res.delete_kobject(fd);
        return OK;
    }
    return ENOEXIST;
}

i64 write(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    if (!is_user_space_pointer_or_null(buffer) || !is_user_space_pointer_or_null(buffer + max_len))
    {
        return EBUFFER;
    }
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto file = obj->get<fs::vfs::file>())
        {
            return file->write(buffer, max_len, flags);
        }
        else
        {
            return ENOTYPE;
        }
    }
    return ENOEXIST;
}

i64 read(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    if (!is_user_space_pointer_or_null(buffer) || !is_user_space_pointer_or_null(buffer + max_len))
    {
        return EBUFFER;
    }

    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto file = obj->get<fs::vfs::file>())
        {
            return file->read(buffer, max_len, flags);
        }
        else
        {
            return ENOTYPE;
        }
    }
    return ENOEXIST;
}

i64 pwrite(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    if (!is_user_space_pointer_or_null(buffer) || !is_user_space_pointer_or_null(buffer + max_len))
    {
        return EBUFFER;
    }
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto file = obj->get<fs::vfs::file>())
        {
            return file->pwrite(offset, buffer, max_len, flags);
        }
        else
        {
            return ENOTYPE;
        }
    }
    return ENOEXIST;
}

i64 pread(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    if (!is_user_space_pointer_or_null(buffer) || !is_user_space_pointer_or_null(buffer + max_len))
    {
        return EBUFFER;
    }
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto file = obj->get<fs::vfs::file>())
        {
            return file->pread(offset, buffer, max_len, flags);
        }
        else
        {
            return ENOTYPE;
        }
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
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto file = obj->get<fs::vfs::file>())
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
        else
        {
            return ENOTYPE;
        }
    }
    return 0;
}

i64 select(u64 fd_count, file_desc *in, file_desc *out, file_desc *err, u64 flags) { return EFAILED; }

i64 pipe(file_desc *fd1, file_desc *fd2)
{
    auto file = fs::vfs::open_pipe();
    if (!file)
        return EFAILED;

    auto &res = task::current_process()->resource;
    auto fdx0 = res.new_kobject(file);
    if (fdx0 == invalid_file_desc)
    {
        return EFAILED;
    }

    auto fdx1 = res.new_kobject(file);
    if (fdx1 == invalid_file_desc)
    {
        res.delete_kobject(fdx0);
        return EFAILED;
    }

    *fd1 = fdx0;
    *fd2 = fdx1;

    return OK;
}

file_desc fifo(const char *path, u64 mode)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;

    auto file = fs::vfs::create_fifo(path, res.root(), res.current(), mode);
    if (!file)
    {
        return EFAILED;
    }
    auto fd = res.new_kobject(file);
    return fd;
}

i64 fcntl(file_desc fd, u64 operator_type, u64 target, u64 attr, u64 *value, u64 size)
{
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (!obj)
    {
        return EFAILED;
    }
    if (obj->is<fs::vfs::file>())
    {
        if (!fs::vfs::fcntl(obj.as<fs::vfs::file>(), operator_type, target, attr, value, size))
        {
            return EFAILED;
        }
        return OK;
    }
    return ENOTYPE;
}

file_desc dup(file_desc fd)
{
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (!obj)
    {
        return EFAILED;
    }
    auto fd2 = res.new_kobject(obj);
    return fd2;
}

int dup2(file_desc fd, file_desc newfd)
{
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (!obj)
    {
        return EFAILED;
    }
    res.set_handle(newfd, obj, true);
    return 0;
}

int istty(int fd)
{
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj && obj->is<fs::vfs::file>())
    {
        if (auto file = obj->get<fs::vfs::file>())
        {
            if (file->get_pseudo() != 0)
            {
                return 0;
            }
        }
    }
    return ENOTTY;
}

int stat(int fd, const char *path, int flags) { return 0; }

int fsync(int fd) { return 0; }
int ftruncate(int fd) { return 0; }

int fallocate(int fd) { return 0; }

BEGIN_SYSCALL

SYSCALL(3, open)
SYSCALL(4, read)
SYSCALL(5, write)
SYSCALL(6, pread)
SYSCALL(7, pwrite)
SYSCALL(8, lseek)
SYSCALL(9, close)
SYSCALL(10, dup)
SYSCALL(11, dup2)
SYSCALL(12, istty);
SYSCALL(13, stat);
SYSCALL(14, fcntl)
SYSCALL(15, fsync);
SYSCALL(16, ftruncate);
SYSCALL(17, fallocate);

SYSCALL(65, pipe)
SYSCALL(66, fifo)

END_SYSCALL
} // namespace syscall
