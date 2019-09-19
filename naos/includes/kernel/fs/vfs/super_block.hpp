#pragma once
#include "common.hpp"
namespace fs::vfs
{
class dentry;
class inode;
class file;

class super_block
{
  protected:
    u64 magic;
    const char *name;
    u64 block_size;
    dentry *root;

  public:
    super_block() = default;
    virtual ~super_block() = default;
    dentry *get_root() { return root; };

    virtual void load() = 0;
    virtual void save() = 0;
    virtual void dirty_inode(inode *node) = 0;

    virtual void fill_dentry(dentry *entry) = 0;
    virtual void save_dentry(dentry *entry) = 0;

    virtual void write_inode(inode *node) = 0;

    virtual file *alloc_file() = 0;
    virtual void dealloc_file(file *f) = 0;
    virtual inode *alloc_inode() = 0;
    virtual void dealloc_inode(inode *node) = 0;
    virtual dentry *alloc_dentry() = 0;
    virtual void dealloc_dentry(dentry *entry) = 0;
};
} // namespace fs::vfs
