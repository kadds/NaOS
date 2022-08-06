#pragma once
#include "../vfs/dentry.hpp"
#include "../vfs/file.hpp"
#include "../vfs/file_system.hpp"
#include "../vfs/inode.hpp"
#include "../vfs/super_block.hpp"
#include "common.hpp"
#include "freelibcxx/hash_map.hpp"

namespace fs::ramfs
{
void init();
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
    bool create_symbolink(vfs::dentry *entry, const char *target) override;
    const char *symbolink() override;
};

class file : public vfs::file
{
  public:
    void flush() override{};

  protected:
    i64 iwrite(i64 &offset, const byte *buffer, u64 size, flag_t flags) override;
    i64 iread(i64 &offset, byte *buffer, u64 max_size, flag_t flags) override;
};

class file_system : public vfs::file_system
{
  public:
    file_system(const char *fsname = "ramfs");
    virtual vfs::super_block *load(const char *device_name, const byte *data, u64 size) override;
    void unload(vfs::super_block *block) override;
};

class super_block : public vfs::super_block
{
  private:
    friend class file_system;
    u64 block_size;
    u64 max_ram_size;
    u64 current_ram_used;
    freelibcxx::hash_map<u64, inode *> inode_map;
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

    handle_t<vfs::file> alloc_file() override;
    inode *alloc_inode() override;
    void dealloc_inode(vfs::inode *node) override;
    dentry *alloc_dentry() override;
    void dealloc_dentry(vfs::dentry *entry) override;

    void add_ram_used(i64 size) { current_ram_used += size; }
    u64 get_current_used() const { return current_ram_used; }
    u64 get_max_ram_size() const { return max_ram_size; }
};
} // namespace fs::ramfs
