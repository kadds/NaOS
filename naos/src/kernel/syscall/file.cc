#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"
namespace syscall
{
file_desc open(const char *filepath, u64 mode, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();

    auto file = fs::vfs::open(filepath, ft->root, mode, flags);
    if (file)
    {
        auto fd = res.new_file_desc(file);
        return fd;
    }
    return -1;
}

bool close(file_desc fd)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        fs::vfs::close(file);
        return true;
    }
    return false;
}

u64 write(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        return file->write(buffer, max_len);
    }
    return 0;
}

u64 read(file_desc fd, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        return file->read(buffer, max_len);
    }
    return 0;
}

u64 pwrite(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        file->move(offset);
        return file->write(buffer, max_len);
    }
    return 0;
}

u64 pread(file_desc fd, i64 offset, byte *buffer, u64 max_len, u64 flags)
{
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        file->move(offset);
        return file->read(buffer, max_len);
    }
    return 0;
}

/// seek file offset
///
/// \param fd the file desc
/// \param offset the offset
/// \param type 0: from current 1: offset of file begin 2: offset of file end
/// \return old offset
u64 lseek(file_desc fd, i64 offset, u64 type)
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

BEGIN_SYSCALL
SYSCALL(10, open)
SYSCALL(11, close)
SYSCALL(12, write)
SYSCALL(13, read)
SYSCALL(14, pwrite)
SYSCALL(15, pread)
SYSCALL(16, lseek)
END_SYSCALL
} // namespace syscall
