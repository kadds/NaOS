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
#include "kernel/trace.hpp"
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
    entry->set_inode(node);
    node->mkdir(entry);
    node->set_type(inode_type_t::directory);
    entry->set_parent(parent);
    parent->add_sub_dir(entry);
    return entry;
}

dentry *create_file(dentry *parent, const char *name, nameidata *idata)
{
    super_block *su_block = parent->get_inode()->get_super_block();
    dentry *entry = su_block->alloc_dentry();
    entry->set_name(name);
    inode *node = su_block->alloc_inode();
    entry->set_inode(node);
    node->create(entry, idata);
    node->set_type(inode_type_t::file);

    entry->set_parent(parent);
    parent->add_sub_file(entry);
    return entry;
}

void delete_file(dentry *entry)
{
    super_block *su_block = entry->get_inode()->get_super_block();

    entry->get_parent()->remove_sub_file(entry);
    entry->get_inode()->remove();
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
        dentry *new_entry = path_walk(new_path, global_root, attribute::auto_create_dir_rescure);
        old->remove_sub_dir(old);
        old->get_inode()->rename(old, new_entry);
        return new_entry;
    }
    else if (old->get_inode()->get_type() == inode_type_t::file)
    {
        dentry *new_entry = path_walk(new_path, global_root, attribute::auto_create_file);
        old->remove_sub_file(old);
        old->get_inode()->rename(old, new_entry);
        return new_entry;
    }
    return nullptr;
}

dentry *path_walk(const char *name, dentry *root, flag_t create_attribute)
{
    nameidata idata;

    while (*name == '/')
        name++;

    dentry *last_entry = root;
    char *name_buffer = memory::NewArray<char>(memory::KernelCommonAllocatorV, 4096);

    while (1)
    {
        char *sub_name = name_buffer;
        dentry *next_entry = nullptr;
        bool end_split = false;
        while (*name != 0)
        {
            if (*name == '/')
            {
                end_split = true;
                name++;
                break;
            }
            *sub_name++ = *name++;
        }
        *sub_name = 0;

        if (end_split)
        {
            if (!last_entry->is_load_child())
                last_entry->load_child();
            next_entry = last_entry->find_child_dir(name_buffer);
            if (next_entry == nullptr)
            {
                if ((create_attribute & attribute::auto_create_dir_rescure) != 0)
                {
                    next_entry = mkdir(last_entry, name_buffer);
                    if (unlikely(next_entry == nullptr))
                        return nullptr;
                }
                else
                {
                    return nullptr;
                }
            }
        }
        else
        {
            if (sub_name - name_buffer == 0) // file name is ""
                return nullptr;
            if (!last_entry->is_load_child())
                last_entry->load_child();
            next_entry = last_entry->find_child_file(name_buffer);
            if (next_entry == nullptr)
            {
                if ((create_attribute & attribute::auto_create_file) != 0)
                {
                    next_entry = create_file(last_entry, name_buffer, &idata);
                    if (unlikely(next_entry == nullptr))
                        return nullptr;
                }
            }

            return next_entry;
        }
        last_entry = next_entry;
    }
    return nullptr;
}

bool mount(file_system *fs, const char *path)
{
    if (util::strcmp(path, "/") == 0)
    {
        // mount global root
        global_root = fs->get_super_block()->get_root();
        data->mount_list.push_back(mount_t("/", nullptr));
    }
    else
    {
        if (unlikely(global_root == nullptr))
        {
            trace::panic("Mount root \"/\" before mount \"", path, "\". File system name: ", fs->get_name());
        }
        dentry *dir = path_walk(path, global_root, 0);
        if (unlikely(dir == nullptr))
        {
            trace::info("Mount point doesn't exist.");
            return false;
        }
        data->mount_list.push_back(mount_t(path, dir));

        auto new_root = fs->get_super_block()->get_root();
        new_root->set_parent(dir->get_parent());
        dir = new_root;
    }
    return true;
}

bool umount(file_system *fs)
{
    for (auto &mnt : data->mount_list)
    {
        if (mnt.su_block == fs->get_super_block())
        {
            dentry *now = path_walk(mnt.mount_point, global_root, 0);

            if (mnt.root == nullptr)
            {
                global_root = mnt.root;
            }
            else
            {
                now = mnt.root;
            }

            return true;
        }
    }
    trace::info("File system ", fs->get_name(), " doesn't mounted.");
    return false;
}

file *open(const char *filepath, flag_t mode, flag_t attr)
{
    dentry *entry = path_walk(filepath, global_root, attr);
    if (entry == nullptr)
        return nullptr;
    file *f = entry->get_inode()->get_super_block()->alloc_file();
    f->open(entry, mode);
    return f;
}

void close(file *f)
{
    f->close();
    f->get_entry()->get_inode()->get_super_block()->dealloc_file(f);
}

void delete_file(const char *file)
{
    dentry *entry = path_walk(file, global_root, 0);
    delete_file(entry);
}

void rename(const char *new_dir, const char *old_dir)
{
    auto entry = path_walk(old_dir, global_root, 0);
    if (entry == nullptr)
        return;
    rename(new_dir, entry);
}

u64 write(file *f, byte *buffer, u64 size) { return f->write(buffer, size); }

u64 read(file *f, byte *buffer, u64 max_size) { return f->read(buffer, max_size); }

void flush(file *f) { f->flush(); }

void mkdir(const char *dir, flag_t attr)
{
    attr &= ~attribute::auto_create_file;
    path_walk(dir, global_root, attr);
}

void rmdir(const char *dir)
{
    dentry *entry = path_walk(dir, global_root, 0);
    if (entry == nullptr)
        return;
    rmdir(entry);
}

u64 size(file *f) { return f->get_entry()->get_inode()->get_size(); }

} // namespace fs::vfs
