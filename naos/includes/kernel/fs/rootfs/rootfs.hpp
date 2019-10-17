#pragma once
#include "kernel/fs/ramfs/ramfs.hpp"

namespace fs::rootfs
{
class dentry : public ramfs::dentry
{
};

class inode : public ramfs::inode
{
};

class file : public ramfs::file
{
};

void init(byte *start_root_image, u64 length);

class super_block : public ramfs::super_block
{
  public:
    super_block()
        : ramfs::super_block(0xFFFFFF){};

    file *alloc_file() override;
    void dealloc_file(vfs::file *f) override;
    inode *alloc_inode() override;
    void dealloc_inode(vfs::inode *node) override;
    dentry *alloc_dentry() override;
    void dealloc_dentry(vfs::dentry *entry) override;
};

class file_system : public ramfs::file_system
{
  public:
    file_system();
    bool load(const char *device_name, byte *data, u64 size) override;
    void unload() override;
};

} // namespace fs::rootfs
