#include "kernel/fs/vfs/file.hpp"
#include "common.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/super_block.hpp"
#include "kernel/fs/vfs/vfs.hpp"

namespace fs::vfs
{

file::~file()
{
    if (entry == nullptr)
        return;

    auto *opened_entry = entry;
    auto *node = opened_entry->get_inode();
    if (node == nullptr)
    {
        entry = nullptr;
        return;
    }

    const bool unlink_on_close = mode & mode::unlink_on_close;
    bool last_open_reference = false;
    if (open_reference)
    {
        last_open_reference = node->release_open_reference();
        open_reference = false;
    }

    // A pseudo object belongs to the inode, so close it only after the last open file
    // description goes away. This also wakes any readers or writers blocked on it.
    if (last_open_reference)
    {
        auto *pd = node->get_pseudo_data();
        if (pd != nullptr)
            pd->close();
    }

    // An unlinked inode is reaped by the last file description that references it.
    // Calling unlink again is intentional: vfs::unlink treats link_count == 0 as a
    // reap request and does not decrement it a second time.
    if (unlink_on_close || node->get_link_count() == 0)
        unlink(opened_entry);

    entry = nullptr;
}

int file::open(dentry *entry, flag_t mode)
{
    if (entry == nullptr || entry->get_inode() == nullptr || open_reference)
        return -1;

    this->entry = entry;
    this->mode = mode;
    this->offset = 0;

    auto *node = entry->get_inode();
    const bool first_open = node->acquire_open_reference();
    if (first_open)
    {
        auto *pd = node->get_pseudo_data();
        if (pd != nullptr)
        {
            const int result = pd->open(mode);
            if (result != 0)
            {
                node->release_open_reference();
                this->entry = nullptr;
                this->mode = 0;
                return result;
            }
        }
    }

    open_reference = true;
    return 0;
}

void file::seek(i64 offset) { this->offset += offset; }

void file::move(i64 where) { this->offset = where; }

i64 file::current_offset() { return offset; }

u64 file::size() const { return entry->get_inode()->get_size(); }

dentry *file::get_entry() const { return entry; }

i64 file::read(byte *ptr, u64 max_size, flag_t flags)
{
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iread(offset, ptr, max_size, flags);
    }
    else
    {
        auto pd = entry->get_inode()->get_pseudo_data();
        if (pd)
            return pd->read_at(offset, ptr, max_size, flags);
        return EFAILED;
    }
    return 0;
}

i64 file::write(const byte *ptr, u64 size, flag_t flags)
{
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iwrite(offset, ptr, size, flags);
    }
    else
    {
        auto pd = entry->get_inode()->get_pseudo_data();
        if (pd)
            return pd->write_at(offset, ptr, size, flags);
        return -1;
    }
    return 0;
}

i64 file::pread(i64 offset, byte *ptr, u64 max_size, flag_t flags)
{
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iread(offset, ptr, max_size, flags);
    }
    else
    {
        auto pd = entry->get_inode()->get_pseudo_data();
        if (pd)
        {
            i64 current_offset = offset;
            return pd->read_at(current_offset, ptr, max_size, flags);
        }
        return -1;
    }
    return 0;
}

i64 file::pwrite(i64 offset, const byte *ptr, u64 size, flag_t flags)
{

    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iwrite(offset, ptr, size, flags);
    }
    else
    {
        auto pd = entry->get_inode()->get_pseudo_data();
        if (pd)
        {
            i64 current_offset = offset;
            return pd->write_at(current_offset, ptr, size, flags);
        }
        return -1;
    }
    return 0;
}

pseudo_t *file::get_pseudo()
{
    if (entry == nullptr || entry->get_inode() == nullptr)
        return nullptr;
    return entry->get_inode()->get_pseudo_data();
}

i64 file::ioctl(ioctl_context &context)
{
    auto pd = get_pseudo();
    if (pd == nullptr)
    {
        return -1;
    }
    return pd->ioctl(context);
}

} // namespace fs::vfs
