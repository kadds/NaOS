#pragma once
#include "common.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/super_block.hpp"

namespace fs::ramfs
{
class super_block;
class inode;
class dentry : public vfs::dentry
{
  public:
    friend class super_block;
    using vfs::dentry::dentry;
};

class inode : public vfs::inode
{
    friend class super_block;
    friend class file;

  private:
    enum node_type
    {
        file,
        directory,
        other,
    };
    node_type type;
    byte *start_ptr;
    u64 ram_size;

  public:
    // create file at fisk
    void create(vfs::dentry *entry, vfs::nameidata *idata) override;
    // remove file in disk
    void remove() override;

    bool has_permission(flag_t pf) override { return true; };

    void mkdir(vfs::dentry *entry) override;
    void rename(vfs::dentry *old_entry, vfs::dentry *new_entry) override;
    void rmdir() override;

    u64 hash() override { return 0; }
};

class file : public vfs::file
{
  public:
    int open(vfs::dentry *entry, flag_t mode) override
    {
        this->entry = entry;
        this->mode = mode;
        return 0;
    };
    int close() override { return 0; };
    u64 write(byte *buffer, u64 size) override;
    u64 read(byte *buffer, u64 max_size) override;
    void flush() override{};
};

class file_system : public vfs::file_system
{
  public:
    file_system(const char *fsname = "ramfs", int version = 1);
    virtual bool load(const char *device_name, byte *data, u64 size) override;
    void unload() override;
};

class super_block : public vfs::super_block
{
  private:
    friend class file_system;
    u64 block_size;
    u64 max_ram_size;
    u64 current_ram_used;

  public:
    super_block(u64 max_ram_size);

    void load() override;
    void save() override;
    void dirty_inode(vfs::inode *node) override;

    void fill_dentry(vfs::dentry *entry) override;
    void save_dentry(vfs::dentry *entry) override;

    void write_inode(vfs::inode *node) override;

    file *alloc_file() override;
    void dealloc_file(vfs::file *f) override;
    inode *alloc_inode() override;
    void dealloc_inode(vfs::inode *node) override;
    dentry *alloc_dentry() override;
    void dealloc_dentry(vfs::dentry *entry) override;

    void add_ram_used(i64 size) { current_ram_used += size; }
    u64 get_current_used() const { return current_ram_used; }
    u64 get_max_ram_size() const { return max_ram_size; }
};
} // namespace fs::ramfs
