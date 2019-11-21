#include "kernel/arch/klib.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"

namespace syscall
{

int create(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return false;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    auto file = fs::vfs::open(pathname, ft->root, ft->current, 0, fs::path_walk_flags::auto_create_file);
    if (file)
    {
        file->close();
        return 0;
    }
    return 1;
}

int access(const char *filepathname, flag_t mode)
{
    if (filepathname == nullptr || !is_user_space_pointer(filepathname))
    {
        return -1;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::access(filepathname, ft->root, ft->current, mode))
        return 0;
    return -2;
}

int mkdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::mkdir(pathname, ft->root, ft->current, 0))
        return 0;
    else
        return -1;
}

int rmdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }
    auto &res = task::current_process()->res_table;
    auto ft = res.get_file_table();
    if (fs::vfs::rmdir(pathname, ft->root, ft->current))
        return 0;
    return -2;
}

int chdir(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    auto entry = fs::vfs::path_walk(pathname, ft->root, ft->current, fs::path_walk_flags::directory);
    if (entry != nullptr)
    {
        ft->current = entry;
        return 0;
    }
    return -2;
}

u64 current_dir(char *pathname, u64 max_len)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname) || !is_user_space_pointer(pathname + max_len))
    {
        return 0;
    }

    auto ft = task::current_process()->res_table.get_file_table();
    return fs::vfs::pathname(ft->root, ft->current, pathname, max_len);
}

int chroot(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    auto entry = fs::vfs::path_walk(pathname, ft->root, ft->current,
                                    fs::path_walk_flags::directory | fs::path_walk_flags::cross_root);
    if (entry != nullptr)
    {
        ft->root = entry;
        return 0;
    }
    return -2;
}

void set_attr(file_desc fd, const char *name, void *value, u64 size, u64 flags) {}

void get_attr(file_desc fd, const char *name, void *value, u64 max_size) {}

int link(const char *src_filepath, const char *target_filepath)
{
    if (src_filepath == nullptr || !is_user_space_pointer(src_filepath))
    {
        return -1;
    }
    if (target_filepath == nullptr || !is_user_space_pointer(target_filepath))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::link(src_filepath, ft->get_path_root(src_filepath), target_filepath, ft->root, ft->current))
        return 0;
    return -1;
}

int unlink(const char *filepath)
{
    if (filepath == nullptr || !is_user_space_pointer(filepath))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::unlink(filepath, ft->root, ft->current))
        return 0;
    return -1;
}

int mount(const char *dev, const char *pathname, const char *fs_type, flag_t flags, const byte *data, u64 size)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }

    if (!is_user_space_pointer(dev))
    {
        return -1;
    }

    if (fs_type == nullptr || !is_user_space_pointer(fs_type))
    {
        return -1;
    }

    if (!is_user_space_pointer(data))
    {
        return -1;
    }

    auto ffs = fs::vfs::get_file_system(fs_type);
    if (ffs == nullptr)
    {
        return -2;
    }
    auto ft = task::current_process()->res_table.get_file_table();

    if (fs::vfs::mount(ffs, dev, pathname, ft->root, ft->current, data, size))
    {
        return 0;
    }
    return -1;
}

int umount(const char *pathname)
{
    if (pathname == nullptr || !is_user_space_pointer(pathname))
    {
        return -1;
    }
    auto ft = task::current_process()->res_table.get_file_table();
    if (fs::vfs::umount(pathname, ft->root, ft->current))
        return 0;
    return -1;
}

BEGIN_SYSCALL
SYSCALL(19, create)
SYSCALL(20, access);
SYSCALL(21, mkdir);
SYSCALL(22, rmdir);
SYSCALL(23, chdir);
SYSCALL(24, current_dir);
SYSCALL(25, chroot);
SYSCALL(26, link);
SYSCALL(27, unlink);
SYSCALL(28, mount);
SYSCALL(29, umount);
END_SYSCALL

} // namespace syscall