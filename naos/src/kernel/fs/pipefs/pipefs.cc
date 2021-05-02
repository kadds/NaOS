#include "kernel/fs/pipefs/pipefs.hpp"
#include "kernel/fs/vfs/vfs.hpp"

namespace fs::pipefs
{

void init()
{
    auto fs = memory::New<file_system>(memory::KernelCommonAllocatorV);
    vfs::register_fs(fs);
    vfs::pipe_block = fs->load(nullptr, nullptr, 0);
}

bool inode::create_symbolink(vfs::dentry *entry, const char *target) { return false; }

const char *inode::symbolink() { return nullptr; }

i64 file::iwrite(const byte *buffer, u64 size, flag_t flags) { return -1; }

i64 file::iread(byte *buffer, u64 max_size, flag_t flags) { return -1; }

file_system::file_system(const char *fsname)
    : vfs::file_system(fsname)
{
}

vfs::super_block *file_system::load(const char *device_name, const byte *data, u64 size)
{
    super_block *su_block = memory::New<super_block>(memory::KernelCommonAllocatorV, this);
    su_block->load();
    return su_block;
}

void file_system::unload(vfs::super_block *su_block)
{
    su_block->save();
    memory::Delete<>(memory::KernelCommonAllocatorV, su_block);
}

super_block::super_block(file_system *fs)
    : vfs::super_block(fs)
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
    inode_map[i->get_index()] = i;
    return i;
}

void super_block::dealloc_inode(vfs::inode *node)
{
    inode_map.remove(node->get_index());
    memory::Delete(memory::KernelCommonAllocatorV, node);
}

dentry *super_block::alloc_dentry() { return memory::New<dentry>(memory::KernelCommonAllocatorV); }

void super_block::dealloc_dentry(vfs::dentry *entry) { memory::Delete(memory::KernelCommonAllocatorV, entry); }

} // namespace fs::pipefs
