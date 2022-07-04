#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"

namespace naos::syscall
{

int readlink(const char *path, byte *buffer, u64 size) { return EFAILED; }

struct list_directory_cursor
{
    u32 size;
    i64 cursor;
};

struct list_directory_result
{
    list_directory_cursor cursor;
    u32 num_files;
    char *files_buffer;
    u32 bytes;
    char *buffer;
};

int list_directory(file_desc fd, list_directory_result *result)
{
    if (result == nullptr || !is_user_space_pointer(result))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto file = res.get_file(fd);
    if (file)
    {
        if (file->get_entry()->get_inode()->get_type() != fs::inode_type_t::directory)
        {
            return EPARAM;
        }
        auto children = file->get_entry()->list_children();
        for (auto child : children)
        {
            child->get_name();
        }
    }
    return ENOEXIST;
}

int rename(const char *src, const char *target)
{
    if (src == nullptr || !is_user_space_pointer(src))
    {
        return EPARAM;
    }
    if (target == nullptr || !is_user_space_pointer(target))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    return !fs::vfs::rename(target, src, ft->root, ft->current);
}

int symbolink(const char *src_path, const char *target_path, flag_t flags)
{
    if (src_path == nullptr || !is_user_space_pointer(src_path))
    {
        return EPARAM;
    }
    if (target_path == nullptr || !is_user_space_pointer(target_path))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::symbolink(src_path, target_path, ft->root, ft->current, flags))
        return 0;
    return -1;
}

u64 create(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::create(pathname, ft->root, ft->current, 0))
    {
        return OK;
    }
    return EFAILED;
}

u64 access(const char *filepathname, flag_t mode)
{
    if (filepathname == nullptr || !is_user_space_pointer(filepathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::access(filepathname, ft->root, ft->current, mode))
        return OK;
    return EFAILED;
}

int mkdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::mkdir(pathname, ft->root, ft->current, 0))
        return OK;
    return EFAILED;
}

int rmdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::rmdir(pathname, ft->root, ft->current))
        return OK;
    return EFAILED;
}

int chdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    auto entry = fs::vfs::path_walk(pathname, ft->root, ft->current, fs::path_walk_flags::directory);
    if (entry != nullptr)
    {
        ft->current = entry;
        return OK;
    }
    return EFAILED;
}

u64 current_dir(char *pathname, u64 max_len)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname) || !is_user_space_pointer(pathname + max_len))
    {
        return EBUFFER;
    }

    auto ft = task::current_process()->res_table.get_file_table();
    return fs::vfs::pathname(ft->root, ft->current, pathname, max_len);
}

int chroot(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();

    auto entry = fs::vfs::path_walk(pathname, ft->root, ft->current,
                                    fs::path_walk_flags::directory | fs::path_walk_flags::cross_root);
    if (entry != nullptr)
    {
        ft->root = entry;
        return OK;
    }
    return EFAILED;
}

void set_attr(file_desc fd, const char *name, void *value, u64 size, u64 flags) {}

void get_attr(file_desc fd, const char *name, void *value, u64 max_size) {}

int link(const char *src, const char *target)
{
    if (src == nullptr || !is_user_space_pointer(src))
    {
        return EPARAM;
    }
    if (target == nullptr || !is_user_space_pointer(target))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::link(src, target, ft->root, ft->current))
        return OK;
    return EFAILED;
}

int unlink(const char *filepath)
{
    if (filepath == nullptr || !is_user_space_pointer(filepath))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::unlink(filepath, ft->root, ft->current))
        return OK;
    return EFAILED;
}

int mount(const char *dev, const char *pathname, const char *fs_type, flag_t flags, const byte *data, u64 size)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }

    if (!is_user_space_pointer(dev))
    {
        return EPARAM;
    }

    if (fs_type == nullptr || !is_user_space_pointer(fs_type))
    {
        return EPARAM;
    }

    if (!is_user_space_pointer(data))
    {
        return EPARAM;
    }

    auto ffs = fs::vfs::get_file_system(fs_type);
    if (ffs == nullptr)
    {
        return EFAILED;
    }
    auto ft = task::current_process()->res_table.get_file_table();

    if (fs::vfs::mount(ffs, dev, pathname, ft->root, ft->current, data, size))
    {
        return OK;
    }
    return EFAILED;
}

int umount(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return EPARAM;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::umount(pathname, ft->root, ft->current))
        return OK;
    return EFAILED;
}

BEGIN_SYSCALL
SYSCALL(15, readlink)
SYSCALL(16, list_directory)
SYSCALL(17, rename)
SYSCALL(18, symbolink)
SYSCALL(19, create)
SYSCALL(20, access)
SYSCALL(21, mkdir)
SYSCALL(22, rmdir)
SYSCALL(23, chdir)
SYSCALL(24, current_dir)
SYSCALL(25, chroot)
SYSCALL(26, link)
SYSCALL(27, unlink)
SYSCALL(28, mount)
SYSCALL(29, umount)
END_SYSCALL

} // namespace syscall