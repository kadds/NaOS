#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/inode.hpp"

namespace fs::vfs
{
int file::open(dentry *entry, flag_t mode)
{
    this->entry = entry;
    this->mode = mode;
    return 0;
}

int file::close() { remove_ref(); }

void file::seek(i64 offset) { pointer_offset += offset; }

void file::move(i64 where) { pointer_offset = where; }

i64 file::current_offset() { return pointer_offset; }

u64 file::size() const { return entry->get_inode()->get_size(); }

dentry *file::get_entry() const { return entry; }
} // namespace fs::vfs
