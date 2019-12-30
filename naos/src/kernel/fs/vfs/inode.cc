#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/timer.hpp"
namespace fs::vfs
{
bool inode::has_permission(flag_t pf, user_id uid, group_id gid)
{
    if (uid == owner)
    {
        return pf & permission;
    }
    else if (gid == group)
    {
        return pf & (permission >> 4);
    }
    else
    {
        return pf & (permission >> 8);
    }
}

void inode::create(vfs::dentry *entry)
{
    entry->set_inode(this);
    link_count = 1;
    ref_count = 0;
    file_size = 0;
    last_write_time = timer::get_high_resolution_time();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::file);
}

void inode::mkdir(vfs::dentry *entry)
{
    entry->set_inode(this);
    link_count = 1;
    ref_count = 0;
    file_size = 0;
    last_write_time = timer::get_high_resolution_time();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::directory);
}

void inode::rename(vfs::dentry *new_entry) {}

void inode::rmdir() { link_count--; }

void inode::link(dentry *old_entry, dentry *new_entry)
{
    if (old_entry == new_entry)
        return;
    new_entry->set_inode(this);
    link_count++;
}

bool inode::unlink(dentry *entry)
{
    if (likely(entry->get_inode() == this))
    {
        link_count--;
        return true;
    }
    return false;
}

bool inode::create_symbolink(dentry *entry, const char *target)
{
    entry->set_inode(this);
    link_count = 1;
    ref_count = 0;
    file_size = 0;
    last_write_time = timer::get_high_resolution_time();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::symbolink);
    return true;
}

bool inode::create_pseudo(dentry *entry, inode_type_t t, u64 size)
{
    entry->set_inode(this);
    link_count = 1;
    ref_count = 0;
    file_size = 0;
    last_write_time = timer::get_high_resolution_time();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(t);
    pseudo_data = nullptr;

    return true;
}

u64 inode::hash() { return ((u64)this) >> 5; }

void inode::update_last_read_time() { last_read_time = timer::get_high_resolution_time(); }

void inode::update_last_write_time() { last_write_time = timer::get_high_resolution_time(); }

void inode::update_last_attr_change_time() { last_attr_change_time = timer::get_high_resolution_time(); }
} // namespace fs::vfs
