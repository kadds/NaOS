#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/mm.hpp"
#include "kernel/fs/vfs/mount.hpp"
#include "kernel/fs/vfs/nameidata.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/super_block.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/linked_list.hpp"
#include "kernel/util/str.hpp"

namespace fs::vfs
{

dentry *global_root = nullptr;
using mount_list_t = util::linked_list<mount_t>;

struct data_t
{
    file_system_list_t fs_list;
    mount_list_t mount_list;
    memory::SlabObjectAllocator dir_entry_allocator, dir_path_allocator;

    data_t()
        : fs_list(memory::MemoryAllocatorV)
        , mount_list(memory::MemoryAllocatorV)
        , dir_entry_allocator(NewSlabGroup(memory::global_object_slab_domain, dir_entry_str, sizeof(dir_entry_str), 0))
        , dir_path_allocator(NewSlabGroup(memory::global_object_slab_domain, dir_path_str, sizeof(dir_path_str), 0))
    {
    }
};

data_t *data;

void init()
{
    trace::debug("VFS init");
    data = memory::New<data_t>(memory::KernelCommonAllocatorV);
}

int register_fs(file_system *fs)
{
    for (auto &lfs : data->fs_list)
    {
        if (util::strcmp(fs->get_name(), lfs->get_name()) == 0)
        {
            return -1;
        }
    }

    data->fs_list.push_back(fs);
    return 0;
}

int unregister_fs(file_system *fs)
{
    for (auto it = data->fs_list.begin(); it != data->fs_list.end(); ++it)
    {
        if (util::strcmp(fs->get_name(), (*it)->get_name()) == 0)
        {
            data->fs_list.remove(it);
            return 0;
        }
    }
    return -1;
}

file_system *get_file_system(const char *name)
{
    for (auto &lfs : data->fs_list)
    {
        if (util::strcmp(name, lfs->get_name()) == 0)
        {
            return lfs;
        }
    }
    return nullptr;
}

i64 rest_dir_name(nameidata &idata)
{
    u64 name_len = 0;
    const char *p_name = idata.last_scan_url_point;
    char *entry_buffer = idata.entry_buffer.get()->name;

    while (*p_name != 0)
    {
        if (name_len != 0)
        {
            const char *old_p_name = p_name;
            while (*p_name == '/')
            {
                p_name++;
                if (*p_name == 0)
                    return name_len;
                else if (*p_name != '/')
                    return -1;
            }
            p_name = old_p_name;
        }
        else if (unlikely(*p_name == '/'))
        {
            p_name++;
            continue;
        }
        *entry_buffer++ = *p_name++;
        name_len++;
        if (name_len >= 512)
            return -1; /// maximun entry size
    }
    *entry_buffer = 0;

    if (name_len <= 0)
        return -1;

    return name_len + 1;
}

i64 rest_file_name(nameidata &idata)
{
    u64 name_len = 0;
    const char *p_name = idata.last_scan_url_point;
    char *entry_buffer = idata.entry_buffer.get()->name;

    while (*p_name != 0)
    {
        if (name_len != 0)
        {
            if (unlikely(*p_name == '/'))
                return -1;
        }
        else if (unlikely(*p_name == '/'))
        {
            p_name++;
            continue;
        }
        *entry_buffer++ = *p_name++;
        name_len++;
        if (name_len >= 512)
            return -1; /// maximun entry size
    }
    *entry_buffer = 0;

    if (name_len <= 0)
        return -1;

    return name_len + 1;
}

dentry *create_dentry(dentry *parent, const char *name, u64 name_len)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    dentry *entry = su_block->alloc_dentry();
    if (unlikely(name_len == 0))
        name_len = util::strlen(name) + 1;

    auto cname = (char *)memory::KernelCommonAllocatorV->allocate(name_len, 1);
    util::memcopy(cname, (const void *)name, name_len);
    entry->set_name(cname);
    entry->set_parent(parent);
    parent->add_child(entry);

    return entry;
}

dentry *mkdir(dentry *parent, const char *name, u64 name_len)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    auto entry = create_dentry(parent, name, name_len);
    inode *node = su_block->alloc_inode();
    node->mkdir(entry);
    return entry;
}

dentry *create_file(dentry *parent, const char *name, u64 name_len, nameidata *idata)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    auto entry = create_dentry(parent, name, name_len);
    inode *node = su_block->alloc_inode();
    node->create(entry);
    return entry;
}

dentry *create_pseudo(inode_type_t type, dentry *parent, const char *name, u64 name_len)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    auto entry = create_dentry(parent, name, name_len);
    inode *node = su_block->alloc_inode();
    node->create_pseudo(entry, type, 0);
    return entry;
}

bool create(const char *path, dentry *root, dentry *cur_dir, flag_t flags)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    auto entry = path_walk(path, root, cur_dir, 0, idata);
    if (entry != nullptr)
    {
        return false; // file exist
    }

    if (flags == 0)
    {
        flags |= create_flags::file;
        // check if path is a directory
        const char *p = idata.last_scan_url_point;
        while (*p != 0)
        {
            if (*p == '/')
            {
                flags |= create_flags::directory;
                break;
            }
            p++;
        }
    }
    i64 name_len = rest_file_name(idata);
    if (name_len <= 0)
        return false; // null name

    if (flags & create_flags::directory)
    {
        entry = mkdir(idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    }
    else if (flags & create_flags::file)
    {
        entry = create_file(idata.last_available_entry, idata.entry_buffer.get()->name, name_len, &idata);
    }
    else if (flags & create_flags::chr)
    {
        entry = create_pseudo(inode_type_t::chr, idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    }
    else if (flags & create_flags::block)
    {
        entry =
            create_pseudo(inode_type_t::block, idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    }
    else if (flags & create_flags::pipe)
    {
        entry = create_pseudo(inode_type_t::pipe, idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    }
    else if (flags & create_flags::socket)
    {
        entry =
            create_pseudo(inode_type_t::socket, idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    }

    if (entry == nullptr)
        return false;

    return true;
}

void rmdir(dentry *entry)
{
    if (entry->get_inode()->get_link_count() == 0)
        return;

    super_block *su_block = entry->get_inode()->get_super_block();
    entry->get_parent()->remove_child(entry);
    entry->get_inode()->rmdir();
    su_block->save_dentry(entry);
    su_block->write_inode(entry->get_inode());
    su_block->dealloc_inode(entry->get_inode());
    su_block->dealloc_dentry(entry);
}

dentry *rename(const char *new_path, dentry *old, dentry *root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *new_entry = path_walk(new_path, root, cur_dir, 0, idata);
    if (new_entry != nullptr)
        return nullptr;

    i64 name_len;
    if (old->get_inode()->get_type() != inode_type_t::directory)
    {
        name_len = rest_file_name(idata);
    }
    else
    {
        name_len = rest_dir_name(idata);
    }

    if (name_len <= 0)
        return nullptr;
    if (old->get_name() != nullptr)
    {
        memory::KernelCommonAllocatorV->deallocate((void *)old->get_name());
    }

    auto cname = (char *)memory::KernelCommonAllocatorV->allocate(name_len, 1);
    util::memcopy(cname, (const void *)idata.entry_buffer.get()->name, name_len);
    old->set_name(cname);
    old->get_parent()->remove_child(old);
    old->set_parent(idata.last_available_entry);
    old->get_parent()->add_child(old);

    old->get_inode()->rename(old);
    return old;
}

u64 next_path_entry_str(const char *&name, char *buffer)
{
    char *b = buffer;
    while (*name != 0)
    {
        if (unlikely(*name == '/'))
        {
            do // skip '/root///next//'
            {
                name++;
            } while (unlikely(*name == '/'));
            break;
        }
        *buffer++ = *name++;
    }
    *buffer = 0;
    return buffer - b;
}

dentry *path_walk(const char *name, dentry *root, dentry *cur_dir, flag_t flags, nameidata &idata)
{
    dentry *prev_entry = cur_dir;
    char *entry_buffer = idata.entry_buffer.get()->name;

    // check and skip root /
    while (*name == '/')
    {
        prev_entry = root;
        name++;
    }
    if (!(flags & path_walk_flags::continue_parse))
    {
        idata.deep = 0;
        idata.symlink_deep = 0;
        idata.last_scan_url_point = name;
        idata.last_available_entry = prev_entry;
    }
    else // try next parse stage
    {
    }

    while (*name != 0)
    {
        idata.last_scan_url_point = name;
        idata.last_available_entry = prev_entry;

        u64 path_entry_len = next_path_entry_str(name, entry_buffer);

        if (!prev_entry->is_load_child())
            prev_entry->load_child();
        idata.deep++;

        if (path_entry_len == 1 && util::strcmp(".", entry_buffer) == 0) // dir ./
            continue;
        else if (path_entry_len == 2 && util::strcmp("..", entry_buffer) == 0) // dir ../
        {
            if (prev_entry != root)
                prev_entry = prev_entry->get_parent();
            else if (path_walk_flags::cross_root & flags)
                if (prev_entry->get_parent() != nullptr)
                    prev_entry = prev_entry->get_parent();
            continue;
        }

        auto next_entry = prev_entry->find_child(entry_buffer);

        if (next_entry == nullptr || next_entry->get_inode()->get_link_count() == 0)
            return nullptr;

        auto next_type = next_entry->get_inode()->get_type();
        if (next_type == fs::inode_type_t::symbolink)
        {
            if (unlikely(flags & path_walk_flags::not_resolve_symbolic_link))
            {
                if (*name == 0) // the last entry is symbolic link entry, return it
                {
                    return next_entry;
                }
                else // else don't resolve symbolic link, can't find file, return null
                {
                    return nullptr;
                }
            }
            u64 old_deep = idata.deep;

            if (idata.symlink_deep++ > 15) // symbolic link deep overflow
                return nullptr;
            next_entry = path_walk(next_entry->get_inode()->symbolink(), root, cur_dir,
                                   flags | path_walk_flags::continue_parse, idata);
            idata.deep = old_deep;
            if (next_entry == nullptr || next_entry->get_inode()->get_link_count() == 0)
                return nullptr;
        }
        prev_entry = next_entry;
    }
    if (prev_entry->get_inode()->get_type() == inode_type_t::file)
    {
        if (flags & path_walk_flags::directory)
            return nullptr;
    }
    else if (prev_entry->get_inode()->get_type() == inode_type_t::directory)
    {
        if (flags & path_walk_flags::file)
            return nullptr;
    }
    return prev_entry;
}

dentry *path_walk(const char *name, dentry *root, dentry *cur_dir, flag_t flags)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);

    return path_walk(name, root, cur_dir, flags, idata);
}

bool mount(file_system *fs, const char *dev, const char *path, dentry *path_root, dentry *cur_dir, const byte *fs_data,
           u64 max_len)
{

    if (util::strcmp(path, "/") == 0 && path_root == nullptr && cur_dir == nullptr)
    {
        if (global_root != nullptr)
        {
            trace::panic("Mount point \"/\" has been mount.");
        }
        // mount global root
        auto su_block = fs->load(dev, fs_data, max_len);
        mount_t mnt("/", nullptr, nullptr, su_block);
        global_root = su_block->get_root();
        data->mount_list.push_back(std::move(mnt));
    }
    else
    {
        if (unlikely(global_root == nullptr))
        {
            trace::panic("Mount root \"/\" before mount \"", path, "\". File system name: ", fs->get_name());
        }

        nameidata idata(&data->dir_entry_allocator, 1, 0);
        dentry *dir = path_walk(path, path_root, cur_dir, path_walk_flags::directory, idata);
        if (unlikely(dir == nullptr))
        {
            trace::warning("Mount point doesn't exist.");
            return false;
        }
        auto su_block = fs->load(dev, fs_data, max_len);
        /// TODO: copy dev string
        mount_t mnt(path, dev, dir, su_block);

        data->mount_list.push_back(std::move(mnt));

        auto new_root = su_block->get_root();
        new_root->set_parent(dir->get_parent());
        new_root->set_name(dir->get_name());
        new_root->set_mount_point();
        dir->get_parent()->remove_child(dir);
        dir->get_parent()->add_child(new_root);
    }
    return true;
}

bool umount(const char *path, dentry *path_root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);

    dentry *dir = path_walk(path, path_root, cur_dir, 0, idata);
    if (dir == nullptr)
        return false;
    auto sb = dir->get_inode()->get_super_block();

    for (auto mnt = data->mount_list.begin(); mnt != data->mount_list.end(); ++mnt)
    {
        if (mnt->su_block == sb)
        {
            if (mnt->mount_entry != nullptr)
            {
                dir->get_parent()->remove_child(sb->get_root());
                dir->get_parent()->add_child(mnt->mount_entry);
                sb->get_root()->set_name("/");
                data->mount_list.remove(mnt);
            }
            else
            {
                trace::panic("Can't umount root.");
            }
            return true;
        }
    }
    trace::info("File system ", sb->get_file_system()->get_name(), " doesn't mounted.");
    return false;
}

file *open(const char *filepath, dentry *root, dentry *cur_dir, flag_t mode, flag_t attr)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry = path_walk(filepath, root, cur_dir, attr, idata);
    while (entry == nullptr)
    {
        if (attr & path_walk_flags::auto_create_file)
        {
            i64 name_len = rest_file_name(idata);
            if (name_len <= 0) // not a file
                return nullptr;

            if (attr & path_walk_flags::directory)
            {
                entry = mkdir(idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
            }
            else
            {
                entry = create_file(idata.last_available_entry, idata.entry_buffer.get()->name, name_len, &idata);
            }
        }
        break;
    }
    if (unlikely(entry == nullptr)) // Can't create file
    {
        return nullptr;
    }

    file *f = entry->get_inode()->get_super_block()->alloc_file();
    f->open(entry, mode);
    return f;
}

bool rename(const char *new_dir, const char *old_dir, dentry *root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);

    auto entry = path_walk(old_dir, root, cur_dir, 0, idata);
    if (entry == nullptr)
        return false;

    return rename(new_dir, entry, root, cur_dir) != nullptr;
}

bool mkdir(const char *dir, dentry *root, dentry *cur_dir, flag_t attr)
{
    attr &= ~path_walk_flags::file;
    attr |= path_walk_flags::directory;
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *e = path_walk(dir, root, cur_dir, attr, idata);
    if (unlikely(e != nullptr)) // exist
        return false;
    i64 name_len = rest_dir_name(idata);
    if (name_len <= 0)
        return false; // null dir name

    mkdir(idata.last_available_entry, idata.entry_buffer.get()->name, name_len);
    return true;
}

bool rmdir(const char *dir, dentry *root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry = path_walk(dir, root, cur_dir, path_walk_flags::directory, idata);
    if (entry == nullptr)
        return false;
    rmdir(entry);
    return true;
}

bool access(const char *pathname, dentry *path_root, dentry *cur_dir, flag_t flags)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry = path_walk(pathname, path_root, cur_dir, 0, idata);
    if (entry == nullptr)
        return false;
    return entry->get_inode()->has_permission(flags, 0, 0);
}

bool link(const char *src, const char *target, dentry *root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry_target = path_walk(target, root, cur_dir, 0, idata);
    if (unlikely(entry_target == nullptr))
        return false;

    nameidata src_idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry = path_walk(src, root, cur_dir, path_walk_flags::file, src_idata);
    if (unlikely(entry != nullptr))
        return false;

    i64 name_len = rest_file_name(src_idata);
    if (unlikely(name_len <= 0))
        return false;

    entry = create_dentry(src_idata.last_available_entry, src_idata.entry_buffer.get()->name, name_len);
    if (unlikely(entry == nullptr))
        return false;

    entry_target->get_inode()->link(entry_target, entry);

    return true;
}

bool unlink(dentry *entry)
{
    auto su = entry->get_inode()->get_super_block();
    auto inode = entry->get_inode();
    inode->unlink(entry);
    /// save to disk
    su->write_inode(inode);
    if (inode->get_link_count() == 0)
    {
        auto type = inode->get_type();
        if (type != fs::inode_type_t::file && type != fs::inode_type_t::directory &&
            type != fs::inode_type_t::symbolink)
        {
            auto pd = inode->get_pseudo_data();
            if (pd)
                memory::Delete<>(memory::KernelCommonAllocatorV, pd);
        }
        if (entry->get_parent())
            entry->get_parent()->remove_child(entry);
        // link count is 0, delete file
        su->dealloc_inode(inode);
        su->dealloc_dentry(entry);
    }
    return true;
}

bool unlink(const char *pathname, dentry *root, dentry *cur_dir)
{
    nameidata idata(&data->dir_entry_allocator, 1, 0);
    dentry *entry = path_walk(pathname, root, cur_dir, path_walk_flags::not_resolve_symbolic_link, idata);
    if (unlikely(entry == nullptr))
        return false;

    return unlink(entry);
}

bool symbolink(const char *src, const char *target, dentry *root, dentry *current, flag_t flags)
{
    nameidata src_idata(&data->dir_entry_allocator, 1, 0);

    dentry *entry = path_walk(src, root, current, 0, src_idata);
    if (unlikely(entry != nullptr))
        return false;
    i64 name_len = rest_file_name(src_idata);
    if (unlikely(name_len <= 0))
        return false;

    entry = create_file(src_idata.last_available_entry, src_idata.entry_buffer.get()->name, name_len, &src_idata);

    if (unlikely(entry == nullptr))
        return false;

    return entry->get_inode()->create_symbolink(entry, target);
}

/// get path name from 'root' to 'current'
u64 pathname(dentry *root, dentry *current, char *path, u64 max_len)
{
    if (unlikely(root == nullptr))
        return 0;

    char *ptr = path;
    i64 rest_len = max_len - 1;
    util::array<dentry *> array(memory::MemoryAllocatorV);

    while (current != root && current != nullptr)
    {
        array.push_back(current);
        current = current->get_parent();
        if (array.size() > 256)
            return 0;
    }
    if (current == nullptr)
        return 0;

    *ptr++ = '/';
    int size = array.size();
    for (int i = size - 1; i >= 0; i--)
    {
        auto entry = array[i];

        int len = util::strlen(entry->get_name());
        rest_len -= len + 1;
        if (rest_len < 0)
        {
            util::memcopy(ptr, entry->get_name(), rest_len + len - 1);
            ptr += rest_len + len - 1;
            *ptr = '\0';
            /// XXX: just return when buffer is too small
            return max_len;
        }
        util::memcopy(ptr, entry->get_name(), len);
        ptr += len;
        *ptr++ = '/';
    }
    if (array.size() > 0)
    {
        ptr--;
    }
    *ptr = '\0';
    return max_len - rest_len;
}

u64 size(file *f) { return f->get_entry()->get_inode()->get_size(); }

bool fcntl(file *f, u64 operator_type, u64 target, u64 attr, u64 *value, u64 size)
{
    auto inode = f->get_entry()->get_inode();
    if (unlikely(size < sizeof(u64)))
        return false;
    if (operator_type == fcntl_type::get)
    {
        if ((f->get_mode() & fs::mode::read) == 0)
        {
            return false;
        }
        switch (attr)
        {
            case fcntl_attr::mtime:
                *value = inode->get_last_write_time();
                break;
            case fcntl_attr::ctime:
                *value = inode->get_last_attr_change_time();
                break;
            case fcntl_attr::atime:
                *value = inode->get_last_read_time();
                break;
            case fcntl_attr::btime:
                *value = inode->get_birth_time();
                break;
            case fcntl_attr::uid:
                *value = inode->get_owner();
                break;
            case fcntl_attr::gid:
                *value = inode->get_group();
                break;
            case fcntl_attr::link_count:
                *value = inode->get_link_count();
                break;
            case fcntl_attr::inode_number:
                *value = inode->get_index();
                break;
            case fcntl_attr::permission:
                *value = inode->get_permission();
                break;
            case fcntl_attr::type:
                *value = (u8)inode->get_type();
                break;
            case fcntl_attr::pseudo_func:
                *value = (u64)inode->get_pseudo_data();
                break;
            default:
                return false;
        }
        return true;
    }
    else if (operator_type == fcntl_type::set)
    {
        if ((f->get_mode() & fs::mode::write) == 0)
        {
            return false;
        }
        switch (attr)
        {
            case fcntl_attr::mtime:
                inode->set_last_write_time(*value);
                break;
            case fcntl_attr::ctime:
                inode->set_last_attr_change_time(*value);
                break;
            case fcntl_attr::atime:
                inode->set_last_read_time(*value);
                break;
            case fcntl_attr::btime:
                inode->set_birth_time(*value);
                break;
            case fcntl_attr::uid:
                inode->set_owner(*value);
                break;
            case fcntl_attr::gid:
                inode->set_group(*value);
                break;
            case fcntl_attr::link_count:
                return false;
            case fcntl_attr::inode_number:
                return false;
            case fcntl_attr::permission:
                inode->set_permission(*value);
                break;
            case fcntl_attr::type:
                return false;
            case fcntl_attr::pseudo_func:
                inode->set_pseudo_data((pseudo_t *)*value);
                break;
            default:
                return false;
        }
        return true;
    }
    return false;
}

super_block *pipe_block;

file *open_pipe()
{
    super_block *su_block = pipe_block;
    dentry *entry = su_block->alloc_dentry();
    if (entry == nullptr)
    {
        return nullptr;
    }

    entry->set_name(nullptr);
    entry->set_parent(nullptr);

    if (unlikely(entry == nullptr))
        return nullptr;

    inode *node = su_block->alloc_inode();
    if (unlikely(node == nullptr))
        return nullptr;
    node->create_pseudo(entry, inode_type_t::pipe, 4096);
    auto ps = memory::New<fs::vfs::pseudo_pipe_t>(memory::KernelCommonAllocatorV, memory::page_size);
    node->set_pseudo_data(ps);

    file *f = su_block->alloc_file();
    f->open(entry, mode::read | mode::write | mode::unlink_on_close);
    return f;
}

file *create_fifo(const char *path, dentry *root, dentry *current, flag_t mode)
{
    nameidata src_idata(&data->dir_entry_allocator, 1, 0);

    dentry *entry = path_walk(path, root, current, 0, src_idata);

    if (entry)
        return nullptr;

    i64 name_len = rest_file_name(src_idata);
    if (unlikely(name_len <= 0))
        return nullptr;

    entry =
        create_pseudo(inode_type_t::pipe, src_idata.last_available_entry, src_idata.entry_buffer.get()->name, name_len);
    if (unlikely(entry == nullptr))
        return nullptr;

    file *f = entry->get_inode()->get_super_block()->alloc_file();
    f->open(entry, mode);
    return f;
}

} // namespace fs::vfs
