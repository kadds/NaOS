#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/mm.hpp"
#include "kernel/fs/vfs/mount.hpp"
#include "kernel/fs/vfs/nameidata.hpp"
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
    data_t()
        : fs_list(memory::KernelCommonAllocatorV)
        , mount_list(memory::KernelCommonAllocatorV)
    {
    }
};

data_t *data;

void init() { data = memory::New<data_t>(memory::KernelCommonAllocatorV); }

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

dentry *mkdir(dentry *parent, const char *name)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    dentry *entry = su_block->alloc_dentry();
    auto cname = memory::NewArray<char>(memory::KernelCommonAllocatorV, util::strlen(name) + 1);
    util::memcopy(cname, (const void *)name, util::strlen(name) + 1);
    entry->set_name(cname);
    inode *node = su_block->alloc_inode();
    node->mkdir(entry);
    entry->set_parent(parent);
    parent->add_sub_dir(entry);
    return entry;
}

dentry *create_file(dentry *parent, const char *name, nameidata *idata)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    dentry *entry = su_block->alloc_dentry();
    auto cname = memory::NewArray<char>(memory::KernelCommonAllocatorV, util::strlen(name) + 1);
    util::memcopy(cname, (const void *)name, util::strlen(name) + 1);
    entry->set_name(cname);
    inode *node = su_block->alloc_inode();
    node->create(entry, idata);
    entry->set_parent(parent);
    parent->add_sub_file(entry);
    return entry;
}

void delete_file(dentry *entry)
{
    super_block *su_block = entry->get_inode()->get_super_block();

    entry->get_parent()->remove_sub_file(entry);
    entry->get_inode()->unlink(entry);
    su_block->dealloc_inode(entry->get_inode());
    su_block->dealloc_dentry(entry);
}

void rmdir(dentry *entry)
{
    super_block *su_block = entry->get_inode()->get_super_block();

    entry->get_parent()->remove_sub_file(entry);
    entry->get_inode()->rmdir();
    su_block->dealloc_inode(entry->get_inode());
    su_block->dealloc_dentry(entry);
}

dentry *rename(const char *new_path, dentry *old)
{
    if (old->get_inode()->get_type() == inode_type_t::directory)
    {
        dentry *new_entry = path_walk(new_path, global_root, global_root, path_walk_flags::auto_create_dir_rescure);
        old->remove_sub_dir(old);
        old->get_inode()->rename(old, new_entry);
        return new_entry;
    }
    else if (old->get_inode()->get_type() == inode_type_t::file)
    {
        dentry *new_entry = path_walk(new_path, global_root, global_root, path_walk_flags::auto_create_file);
        old->remove_sub_file(old);
        old->get_inode()->rename(old, new_entry);
        return new_entry;
    }
    return nullptr;
}

dentry *path_walk(const char *name, dentry *root, dentry *cur_dir, flag_t flags)
{
    nameidata idata;
    dentry *prev_entry = cur_dir;

    while (*name == '/')
    {
        prev_entry = root;
        name++;
    }

    memory::MemoryView<char> path_buffer(memory::KernelBuddyAllocatorV, 1, 0);
    while (*name != 0)
    {
        char *offset_ptr = path_buffer.get();
        dentry *next_entry = nullptr;
        bool separator = false;
        while (*name != 0)
        {
            if (unlikely(*name == '/'))
            {
                do // skip '/root///next//'
                {
                    name++;
                } while (unlikely(*name == '/'));
                separator = true;
                break;
            }
            *offset_ptr++ = *name++;
        }

        bool is_dir = true;
        if (unlikely(*name == 0)) // last section
        {
            if (flags & path_walk_flags::file)
                is_dir = false;
            else if (flags & path_walk_flags::directory)
                is_dir = true;
            else
                is_dir = separator;
        }

        *offset_ptr = 0;
        int section_len = offset_ptr - path_buffer.get();

        if (!prev_entry->is_load_child())
            prev_entry->load_child();

        if (is_dir)
        {
            if (section_len == 2 && util::strcmp(".", path_buffer.get()) == 0)
                continue;
            else if (section_len == 3 && util::strcmp("..", path_buffer.get()) == 0)
            {
                if (prev_entry != root)
                    prev_entry = prev_entry->get_parent();
                continue;
            }

            next_entry = prev_entry->find_child_dir(path_buffer.get());
            if (next_entry == nullptr)
            {
                if ((flags & path_walk_flags::auto_create_dir_rescure) != 0)
                {
                    next_entry = mkdir(prev_entry, path_buffer.get());
                    if (unlikely(next_entry == nullptr))
                        return nullptr;
                }
                else
                    return nullptr;
            }
        }
        else
        {
            next_entry = prev_entry->find_child_file(path_buffer.get());
            if (next_entry == nullptr)
            {
                if ((flags & path_walk_flags::auto_create_file) != 0)
                {
                    next_entry = create_file(prev_entry, path_buffer.get(), &idata);
                    if (unlikely(next_entry == nullptr))
                        return nullptr;
                }
            }

            return next_entry;
        }
        prev_entry = next_entry;
    }
    return prev_entry;
}

bool mount(file_system *fs, const char *dev, const char *path, dentry *path_root, dentry *cur_dir)
{
    if (util::strcmp(path, "/") == 0)
    {
        if (global_root != nullptr)
        {
            trace::panic("Mount point \"/\" has been mount.");
        }
        // mount global root
        auto su_block = fs->load(dev, 0, 0);
        mount_t mnt("/", nullptr, nullptr, su_block);
        global_root = su_block->get_root();
        data->mount_list.push_back(mnt);
    }
    else
    {
        if (unlikely(global_root == nullptr))
        {
            trace::panic("Mount root \"/\" before mount \"", path, "\". File system name: ", fs->get_name());
        }
        dentry *dir = path_walk(path, path_root, cur_dir, path_walk_flags::directory);
        if (unlikely(dir == nullptr))
        {
            trace::warning("Mount point doesn't exist.");
            return false;
        }
        auto su_block = fs->load(dev, 0, 0);
        /// TODO: copy dev string
        mount_t mnt(path, dev, dir, su_block);

        data->mount_list.push_back(mnt);

        auto new_root = su_block->get_root();
        new_root->set_parent(dir->get_parent());
        new_root->set_mount_point();
        dir->get_parent()->remove_sub_dir(dir);
        dir->get_parent()->add_sub_dir(new_root);
    }
    return true;
}

bool umount(const char *path, dentry *path_root, dentry *cur_dir)
{
    dentry *dir = path_walk(path, path_root, cur_dir, 0);
    auto sb = dir->get_inode()->get_super_block();

    for (auto mnt = data->mount_list.begin(); mnt != data->mount_list.end(); ++mnt)
    {
        if (mnt->su_block == sb)
        {
            if (mnt->mount_entry != nullptr)
            {
                dir->get_parent()->remove_sub_dir(sb->get_root());
                dir->get_parent()->add_sub_dir(mnt->mount_entry);
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
    dentry *entry = path_walk(filepath, root, cur_dir, attr);
    if (entry == nullptr)
        return nullptr;
    file *f = entry->get_inode()->get_super_block()->alloc_file();
    f->open(entry, mode);
    return f;
}

void close(file *f) { f->close(); }

void delete_file(const char *file)
{
    dentry *entry = path_walk(file, global_root, global_root, 0);
    delete_file(entry);
}

void rename(const char *new_dir, const char *old_dir)
{
    auto entry = path_walk(old_dir, global_root, global_root, 0);
    if (entry == nullptr)
        return;
    rename(new_dir, entry);
}

bool mkdir(const char *dir, dentry *root, dentry *cur_dir, flag_t attr)
{
    attr &= ~path_walk_flags::auto_create_file;
    attr |= path_walk_flags::auto_create_dir_rescure | path_walk_flags::directory;
    dentry *e = path_walk(dir, root, cur_dir, attr);
    return e != nullptr;
}

bool rmdir(const char *dir, dentry *root, dentry *cur_dir)
{
    dentry *entry = path_walk(dir, root, cur_dir, path_walk_flags::directory);
    if (entry == nullptr)
        return false;
    rmdir(entry);
    return true;
}

bool access(const char *pathname, dentry *path_root, dentry *cur_dir, flag_t flags)
{
    dentry *entry = path_walk(pathname, path_root, cur_dir, 0);
    if (entry == nullptr)
        return false;
    return entry->get_inode()->has_permission(flags, 0, 0);
}

bool link(const char *pathname, dentry *path_root, const char *target_path, dentry *target_root, dentry *cur_dir)
{
    dentry *entry_target = path_walk(target_path, target_root, cur_dir, path_walk_flags::directory);
    if (entry_target == nullptr)
        return false;

    dentry *entry = path_walk(pathname, path_root, cur_dir, path_walk_flags::directory);
    if (entry == nullptr)
        return false;
    entry->get_inode()->link(entry, entry_target);
    return true;
}

bool unlink(const char *pathname, dentry *root, dentry *cur_dir)
{
    dentry *entry = path_walk(pathname, root, cur_dir, path_walk_flags::directory);
    if (entry == nullptr)
        return false;
    entry->get_inode()->unlink(entry);
    return true;
}

u64 pathname(dentry *root, dentry *current, char *path, u64 max_len)
{
    if (unlikely(root == nullptr))
        return 0;

    char *ptr = path;
    u64 rest_len = max_len - 1;
    util::array<dentry *> array(memory::KernelCommonAllocatorV);

    while (current != root && current != nullptr)
    {
        array.push_back(current);
        current = current->get_parent();
        if (array.size() > 256)
            return 0;
    }
    if (current == nullptr)
        return 0;

    int size = array.size();
    for (int i = size - 1; i >= 0; i--)
    {
        auto entry = array[i];
        *ptr++ = '/';
        int len = util::strcopy(ptr, entry->get_name(), rest_len);
        rest_len -= len;
        ptr += len;
        if (rest_len <= 1)
            return max_len;
    }
    *ptr = 0;
    return max_len - rest_len;
}

u64 size(file *f) { return f->get_entry()->get_inode()->get_size(); }

} // namespace fs::vfs
