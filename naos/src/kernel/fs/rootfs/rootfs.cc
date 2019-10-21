#include "kernel/fs/rootfs/rootfs.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/str.hpp"
namespace fs::rootfs
{
file_system *global_root_file_system;

struct rootfs_head
{
    u64 magic;
    u64 version;
    u64 file_count;
};

void init(byte *start_root_image, u64 size)
{
    if (start_root_image == nullptr || size == 0)
    {
        trace::panic("Can't find root image.");
    }
    global_root_file_system = memory::New<file_system>(memory::KernelCommonAllocatorV);
    vfs::register_fs(global_root_file_system);
    global_root_file_system->load(nullptr, start_root_image, size);
    vfs::mount(global_root_file_system, "/");
    rootfs_head *head = (rootfs_head *)start_root_image;
    if (head->magic != 0xF5EEEE5F)
        trace::panic("rfsimg magic head is invalid");

    if (head->version == 1)
    {
        byte *start_of_file = start_root_image + sizeof(rootfs_head);
        for (u64 i = 0; i < head->file_count; i++)
        {
            char *file_name = (char *)start_of_file;
            int len = util::strlen(file_name);
            u64 data_size = *(u64 *)(start_of_file + len + 1);
            byte *data = start_of_file + len + 1 + sizeof(u64);
            auto file = vfs::open(file_name, vfs::mode::write | vfs::mode::bin,
                                  vfs::attribute::auto_create_file | vfs::attribute::auto_create_dir_rescure);
            file->write(data, data_size);
            vfs::close(file);

            start_of_file = data + data_size;
        }
        if ((u64)(start_of_file - start_root_image) != size)
            trace::panic("rfsimg has incorrect content!");
    }
}

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

file_system::file_system()
    : ramfs::file_system("rootfs", 1)
{
}

bool file_system::load(const char *device_name, byte *data, u64 size)
{
    super_block *su_block = memory::New<super_block>(memory::KernelCommonAllocatorV);
    this->su_block = su_block;
    su_block->load();
    return true;
}
void file_system::unload()
{
    su_block->save();
    memory::Delete<>(memory::KernelCommonAllocatorV, su_block);
}
} // namespace fs::rootfs
