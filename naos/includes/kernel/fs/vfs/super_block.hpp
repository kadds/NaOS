#pragma once
#include "common.hpp"
#include "defines.hpp"
namespace fs::vfs
{

class super_block
{
  protected:
    u64 magic;
    const char *name;
    dentry *root;
    file_system *fs;

  public:
    super_block(file_system *fs)
        : root(nullptr)
        , fs(fs){};

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

    file_system *get_file_system() { return fs; }
};
} // namespace fs::vfs
