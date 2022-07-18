#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"
namespace fs::ramfs
{

void init() { vfs::register_fs(memory::New<file_system>(memory::KernelCommonAllocatorV)); }

bool inode::create_symbolink(vfs::dentry *entry, const char *target)
{
    bool ok = vfs::inode::create_symbolink(entry, target);
    /// TODO: save
    if (ok)
    {
        file f;
        f.open(entry, mode::write);
        u64 len = util::strlen(target) + 1;
        f.write((const byte *)target, len, 0);
    }
    return ok;
}

const char *inode::symbolink() { return (const char *)start_ptr; }

i64 file::iwrite(i64 &offset, const byte *buffer, u64 size, flag_t flags)
{
    inode *node = (inode *)entry->get_inode();
    super_block *sublock = (super_block *)node->su_block;

    if (unlikely(offset + size >= node->ram_size || node->start_ptr == nullptr))
    {
        sublock->add_ram_used(-node->ram_size);
        if (node->start_ptr != nullptr)
        {
            memory::KernelBuddyAllocatorV->deallocate(node->start_ptr);
        }
        auto file_size = offset + size;
        auto ram_size = (file_size + memory::page_size - 1) & ~(memory::page_size - 1);

        if (sublock->get_current_used() + ram_size < sublock->get_max_ram_size())
        {
            node->file_size = file_size;
            node->ram_size = ram_size;
            node->start_ptr = (byte *)memory::KernelBuddyAllocatorV->allocate(node->ram_size, 0);
            sublock->add_ram_used(node->ram_size);
        }
        else
        {
            return 0;
        }
    }

    util::memcopy(node->start_ptr + offset, buffer, size);
    offset += size;
    return size;
}

i64 file::iread(i64 &offset, byte *buffer, u64 max_size, flag_t flags)
{
    inode *node = (inode *)entry->get_inode();
    if (max_size > node->file_size - offset)
        max_size = node->file_size - offset;

    if (node->ram_size < offset + max_size)
    {
        if (node->ram_size < (uint64_t)offset)
        {
            max_size = 0;
        }
        else
        {
            max_size = node->ram_size - offset;
        }
    }

    util::memcopy(buffer, node->start_ptr + offset, max_size);
    offset += max_size;
    return max_size;
}

file_system::file_system(const char *fsname)
    : vfs::file_system(fsname)
{
}

vfs::super_block *file_system::load(const char *device_name, const byte *data, u64 size)
{
    super_block *su_block = memory::New<super_block>(memory::KernelCommonAllocatorV, size, this);
    su_block->load();
    return su_block;
}

void file_system::unload(vfs::super_block *su_block)
{
    su_block->save();
    memory::Delete<>(memory::KernelCommonAllocatorV, su_block);
}

super_block::super_block(u64 max_ram_size, file_system *fs)
    : vfs::super_block(fs)
    , block_size(memory::page_size)
    , max_ram_size(max_ram_size)
    , current_ram_used(0)
    , inode_map(memory::MemoryAllocatorV, 20, 500)
    , last_inode_index(0)
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
    r->node->mkdir(root);
}

void super_block::save() {}

void super_block::dirty_inode(vfs::inode *node) {}

void super_block::fill_dentry(vfs::dentry *entry) { entry->set_loaded(true); }

void super_block::save_dentry(vfs::dentry *entry) {}

void super_block::write_inode(vfs::inode *node) {}

vfs::inode *super_block::get_inode(u64 node_index)
{
    inode *node = nullptr;
    inode_map.get(node_index, &node);
    return node;
}

file *super_block::alloc_file() { return memory::New<file>(memory::KernelCommonAllocatorV); }

void super_block::dealloc_file(vfs::file *f) { memory::Delete(memory::KernelCommonAllocatorV, f); }

inode *super_block::alloc_inode()
{
    inode *i = memory::New<inode>(memory::KernelCommonAllocatorV);
    i->set_super_block(this);
    i->set_index(last_inode_index++);
    inode_map.insert(i->get_index(), i);
    return i;
}

void super_block::dealloc_inode(vfs::inode *node)
{
    inode_map.remove(node->get_index());
    memory::Delete(memory::KernelCommonAllocatorV, node);
}

dentry *super_block::alloc_dentry() { return memory::New<dentry>(memory::KernelCommonAllocatorV); }

void super_block::dealloc_dentry(vfs::dentry *entry) { memory::Delete(memory::KernelCommonAllocatorV, entry); }

} // namespace fs::ramfs
