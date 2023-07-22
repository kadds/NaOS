#include "kernel/fs/vfs/file.hpp"
#include "common.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/super_block.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/errno.hpp"

namespace fs::vfs
{

file::~file()
{
    auto type = entry->get_inode()->get_type();
    if (type != fs::inode_type_t::file && type != fs::inode_type_t::directory && type != fs::inode_type_t::symbolink)
    {
        // auto pd = entry->get_inode()->get_pseudo_data();
        // if (pd)
        //     pd->close();
    }
    auto entry = this->entry;
    // auto su = entry->get_inode()->get_super_block();
    if (mode & mode::unlink_on_close)
    {
        unlink(entry);
    }
    return;
}

int file::open(dentry *entry, flag_t mode)
{
    this->entry = entry;
    this->mode = mode;
    this->offset = 0;
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
            return pd->read(ptr, max_size, flags);
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
            return pd->write(ptr, size, flags);
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
            return pd->read(ptr, max_size, flags);
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
            return pd->write(ptr, size, flags);
        return -1;
    }
    return 0;
}

pseudo_t *file::get_pseudo() { return entry->get_inode()->get_pseudo_data(); }

} // namespace fs::vfs
