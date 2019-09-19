#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/util/memory.hpp"

namespace fs::ramfs
{

void inode::create(vfs::dentry *entry, vfs::nameidata *idata) {}

void inode::remove() {}

void inode::mkdir(vfs::dentry *entry) {}

void inode::rename(vfs::dentry *old_entry, vfs::dentry *new_entry) {}

void inode::rmdir() {}

u64 file::write(byte *buffer, u64 size)
{
    inode *node = (inode *)entry->get_inode();
    super_block *sublock = (super_block *)node->su_block;

    if (unlikely(pointer_offset + size >= node->ram_size || node->start_ptr == nullptr))
    {
        sublock->add_ram_used(-node->ram_size);
        if (node->start_ptr != nullptr)
        {
            memory::KernelBuddyAllocatorV->deallocate(node->start_ptr);
        }
        node->file_size = pointer_offset + size;
        node->ram_size = (node->file_size + memory::page_size - 1) & ~(memory::page_size - 1);
        if (sublock->get_current_used() + node->ram_size < sublock->get_max_ram_size())
        {
            node->start_ptr = (byte *)memory::KernelBuddyAllocatorV->allocate(node->ram_size, 0);
            sublock->add_ram_used(node->ram_size);
        }
        else
        {
            return 0;
        }
    }

    util::memcopy(node->start_ptr + pointer_offset, buffer, size);
    pointer_offset += size;
    return size;
}
u64 file::read(byte *buffer, u64 max_size)
{
    inode *node = (inode *)entry->get_inode();
    if (max_size > node->file_size - pointer_offset)
        max_size = node->file_size - pointer_offset;

    util::memcopy(buffer, node->start_ptr + pointer_offset, max_size);
    pointer_offset += max_size;
    return max_size;
}

file_system::file_system(const char *fsname, int version)
    : vfs::file_system(fsname, version)
{
}

bool file_system::load(const char *device_name, byte *data, u64 size)
{
    super_block *su_block = memory::New<super_block>(memory::KernelCommonAllocatorV, size);
    this->su_block = su_block;
    su_block->load();
    return true;
}

void file_system::unload()
{
    su_block->save();
    memory::Delete<>(memory::KernelCommonAllocatorV, su_block);
}

super_block::super_block(u64 max_ram_size)
    : block_size(memory::page_size)
    , max_ram_size(max_ram_size)
    , current_ram_used(0)
{
}

void super_block::load()
{
    if (root == nullptr)
        root = alloc_dentry();

    dentry *r = (dentry *)root;
    root->set_name("/");
    root->set_parent(nullptr);

    r->node = alloc_inode();
}

void super_block::save() {}

void super_block::dirty_inode(vfs::inode *node) {}

void super_block::fill_dentry(vfs::dentry *entry) { entry->set_loaded(true); }

void super_block::save_dentry(vfs::dentry *entry) {}

void super_block::write_inode(vfs::inode *node) {}

file *super_block::alloc_file() { return memory::New<file>(memory::KernelCommonAllocatorV); }

void super_block::dealloc_file(vfs::file *f) { memory::Delete(memory::KernelCommonAllocatorV, f); }

inode *super_block::alloc_inode()
{
    inode *i = memory::New<inode>(memory::KernelCommonAllocatorV);
    i->set_super_block(this);
    return i;
}

void super_block::dealloc_inode(vfs::inode *node) { memory::Delete(memory::KernelCommonAllocatorV, node); }

dentry *super_block::alloc_dentry() { return memory::New<dentry>(memory::KernelCommonAllocatorV); }

void super_block::dealloc_dentry(vfs::dentry *entry) { memory::Delete(memory::KernelCommonAllocatorV, entry); }

} // namespace fs::ramfs
