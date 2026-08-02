#include "kernel/fs/vfs/inode.hpp"
#include "kernel/clock.hpp"
#include "kernel/fs/vfs/dentry.hpp"
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
    link_count.store(1);
    ref_count.store(0);
    file_size = 0;
    last_write_time = timeclock::get_current_clock();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::file);
}

void inode::mkdir(vfs::dentry *entry)
{
    entry->set_inode(this);
    link_count.store(1);
    ref_count.store(0);
    file_size = 0;
    last_write_time = timeclock::get_current_clock();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::directory);
}

void inode::rename(vfs::dentry *new_entry) {}

void inode::rmdir()
{
    auto count = link_count.load();
    while (count != 0 && !link_count.compare_exchange_weak(count, count - 1))
    {
    }
}

void inode::link(dentry *old_entry, dentry *new_entry)
{
    if (old_entry == new_entry)
        return;
    new_entry->set_inode(this);
    link_count.fetch_add(1);
}

bool inode::unlink(dentry *entry)
{
    if (likely(entry->get_inode() == this))
    {
        auto count = link_count.load();
        while (count != 0 && !link_count.compare_exchange_weak(count, count - 1))
        {
        }
        return true;
    }
    return false;
}

bool inode::create_symbolink(dentry *entry, const char *target)
{
    entry->set_inode(this);
    link_count.store(1);
    ref_count.store(0);
    file_size = 0;
    last_write_time = timeclock::get_current_clock();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(inode_type_t::symbolink);

    return true;
}

bool inode::create_pseudo(dentry *entry, inode_type_t t, u64 size)
{
    entry->set_inode(this);
    link_count.store(1);
    ref_count.store(0);
    file_size = size;
    last_write_time = timeclock::get_current_clock();
    last_read_time = last_write_time;
    last_attr_change_time = last_write_time;
    birth_time = last_write_time;
    set_type(t);
    pseudo_data = nullptr;

    return true;
}

bool inode::acquire_open_reference() { return ref_count.fetch_add(1) == 0; }

bool inode::release_open_reference()
{
    auto count = ref_count.load();
    while (count != 0 && !ref_count.compare_exchange_weak(count, count - 1))
    {
    }
    return count == 1;
}

u64 inode::hash() { return ((u64)this) >> 5; }

void inode::update_last_read_time() { last_read_time = timeclock::get_current_clock(); }

void inode::update_last_write_time() { last_write_time = timeclock::get_current_clock(); }

void inode::update_last_attr_change_time() { last_attr_change_time = timeclock::get_current_clock(); }
} // namespace fs::vfs
