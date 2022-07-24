#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/handle.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"

namespace naos::syscall
{

int readlink(const char *path, byte *buffer, u64 size) { return EFAILED; }

struct dentry
{
    u64 inode;
    u32 name_offset; // offset of entry_name_buffer
    u32 type;
};

struct dentries
{
    i64 offset;
    char *entry_name_buffer;
    u64 buffer_size;
    u64 entry_count;
    dentry entry[0];
};

class list_entries : public kobject
{
  public:
    list_entries(fs::vfs::dentry *entry)
        : kobject(type_of())
        , entries(memory::KernelCommonAllocatorV)
    {
        if (!entry->is_load_child())
        {
            entry->load_child();
        }
        for (auto &child : entry->list_children())
        {
            entries.push_back(child);
        }
    }

    void read(dentries *dentries)
    {
        u64 offset = dentries->offset;
        u64 entry_limit = dentries->entry_count;
        u64 buffer_limit = dentries->buffer_size;
        char *buf = dentries->entry_name_buffer;
        u64 buffer_used = 0;
        u64 i = 0;
        for (; offset < entries.size() && i < entry_limit; offset++, i++)
        {
            auto name = entries[offset]->get_name();
            auto len = util::strlen(name) + 1;
            if (len + buffer_used < buffer_limit)
            {
                auto &d = dentries->entry[i];
                d.type = 0;
                d.inode = 1;
                d.name_offset = buffer_used;
                util::memcopy(buf + buffer_used, name, len);
            }
            else
            {
                break;
            }
        }
        dentries->offset = offset;
        dentries->entry_count = i;
    }

    static type_e type_of() { return type_e::list_entries; }

  private:
    util::array<fs::vfs::dentry *> entries;
};

int open_dir(const char *path)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    auto file = fs::vfs::open(path, res.root(), res.current(), fs::mode::read, fs::path_walk_flags::directory);
    if (!file)
    {
        return ENOEXIST;
    }

    auto handle = handle_t<list_entries>::make(file->get_entry());
    auto fd = res.new_kobject(handle);
    return fd;
}

int list_dir(file_desc fd, dentries *entries)
{
    if (!is_user_space_pointer_or_null(entries))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer(entries->entry_name_buffer))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    auto obj = res.get_kobject(fd);
    if (obj)
    {
        if (auto l = obj->get<list_entries>())
        {
            l->read(entries);
            return 0;
        }
        else
        {
            return ENOTYPE;
        }
    }
    return ENOEXIST;
}

int rename(const char *src, const char *target)
{
    if (!is_user_space_pointer_or_null(src))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer_or_null(target))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    return !fs::vfs::rename(target, src, res.root(), res.current());
}

int symbolink(const char *src_path, const char *target_path, flag_t flags)
{
    if (!is_user_space_pointer_or_null(src_path))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer_or_null(target_path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::symbolink(src_path, target_path, res.root(), res.current(), flags))
        return 0;
    return -1;
}

u64 create(const char *pathname)
{
    if (!is_user_space_pointer_or_null(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::create(pathname, res.root(), res.current(), 0))
    {
        return OK;
    }
    return EFAILED;
}

u64 access(const char *filename, flag_t mode)
{
    if (!is_user_space_pointer_or_null(filename))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::access(filename, res.root(), res.current(), mode))
    {
        return OK;
    }
    return EFAILED;
}

int mkdir(const char *pathname)
{
    if (!is_user_space_pointer_or_null(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::mkdir(pathname, res.root(), res.current(), 0))
        return OK;
    return EFAILED;
}

int rmdir(const char *pathname)
{
    if (!is_user_space_pointer_or_null(pathname))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::rmdir(pathname, res.root(), res.current()))
        return OK;
    return EFAILED;
}

int link(const char *src, const char *target)
{
    if (!is_user_space_pointer_or_null(src))
    {
        return EPARAM;
    }
    if (!is_user_space_pointer_or_null(target))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::link(src, target, res.root(), res.current()))
        return OK;
    return EFAILED;
}

int unlink(file_desc fd, const char *path, flag_t flag)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fd >= 0)
    {
        auto obj = res.get_kobject(fd);
        if (obj) {
            if (auto f = obj->get<fs::vfs::file>())
            {
                if (fs::vfs::unlink(path, res.root(), f->get_entry()))
                    return OK;
            }
            else
            {
                return ENOTYPE;
            }
        }
    } else {
        if (fs::vfs::unlink(path, res.root(), res.current()))
            return OK;
    }
    return EFAILED;
}

int mount(const char *dev, const char *path, const char *fs_type, flag_t flags, const byte *data, u64 size)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }

    if (!is_user_space_pointer(dev))
    {
        return EPARAM;
    }

    if (!is_user_space_pointer_or_null(fs_type))
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
    auto &res = task::current_process()->resource;

    if (fs::vfs::mount(ffs, dev, path, res.root(), res.current(), data, size))
    {
        return OK;
    }
    return EFAILED;
}

int umount(const char *path)
{
    if (!is_user_space_pointer_or_null(path))
    {
        return EPARAM;
    }
    auto &res = task::current_process()->resource;
    if (fs::vfs::umount(path, res.root(), res.current()))
        return OK;
    return EFAILED;
}

BEGIN_SYSCALL
SYSCALL(18, open_dir)
SYSCALL(19, list_dir)
SYSCALL(20, access)

SYSCALL(21, readlink)
SYSCALL(22, unlink);
SYSCALL(23, mkdir)
SYSCALL(24, rmdir)
SYSCALL(25, rename)
SYSCALL(26, create)
SYSCALL(27, link)
SYSCALL(28, symbolink)
SYSCALL(29, mount)
SYSCALL(30, umount)
END_SYSCALL

} // namespace syscall