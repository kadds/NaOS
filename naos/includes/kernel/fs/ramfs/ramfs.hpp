#pragma once
#include "common.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/super_block.hpp"
#include "kernel/util/hash_map.hpp"

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
    byte *start_ptr;
    u64 ram_size;

  public:
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
    file_system(const char *fsname = "ramfs");
    virtual vfs::super_block *load(const char *device_name, byte *data, u64 size) override;
    void unload(vfs::super_block *block) override;
};
struct member_hash
{
    u64 operator()(u64 i) { return i; }
};
class super_block : public vfs::super_block
{
  private:
    friend class file_system;
    u64 block_size;
    u64 max_ram_size;
    u64 current_ram_used;
    util::hash_map<u64, inode *, member_hash> inode_map;
    int last_inode_index;

  public:
    super_block(u64 max_ram_size, file_system *fs);

    void load() override;
    void save() override;
    void dirty_inode(vfs::inode *node) override;

    void fill_dentry(vfs::dentry *entry) override;
    void save_dentry(vfs::dentry *entry) override;

    void write_inode(vfs::inode *node) override;
    vfs::inode *get_inode(u64 node_index);

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
