#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/super_block.hpp"
namespace fs::vfs
{

int file::open(dentry *entry, flag_t mode)
{
    this->entry = entry;
    this->mode = mode;
    add_ref();
    return 0;
}

void file::close()
{
    remove_ref();
    if (ref_count == 0)
    {
        auto type = entry->get_inode()->get_type();
        if (type != fs::inode_type_t::file && type != fs::inode_type_t::directory &&
            type != fs::inode_type_t::symbolink)
        {
            auto pd = entry->get_inode()->get_pseudo_data();
            if (pd)
                pd->close();
        }
        entry->get_inode()->get_super_block()->dealloc_file(this);
    }
    return;
}

file *file::clone()
{
    file *f = entry->get_inode()->get_super_block()->alloc_file();
    if (!f)
        return nullptr;

    f->entry = entry;
    f->mode = mode;
    f->add_ref();
    f->pointer_offset = pointer_offset;
    return f;
}

void file::seek(i64 offset) { pointer_offset += offset; }

void file::move(i64 where) { pointer_offset = where; }

i64 file::current_offset() { return pointer_offset; }

u64 file::size() const { return entry->get_inode()->get_size(); }

dentry *file::get_entry() const { return entry; }

bool wait_buf(u64 data)
{
    auto buf = (util::circular_buffer<byte> *)data;
    return buf->data_size() != 0;
}

u64 file::read(byte *ptr, u64 max_size, flag_t flags)
{
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iread(ptr, max_size, flags);
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

u64 file::write(const byte *ptr, u64 size, flag_t flags)
{
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
    {
        return iwrite(ptr, size, flags);
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
