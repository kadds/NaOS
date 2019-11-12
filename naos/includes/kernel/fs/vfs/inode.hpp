#pragma once
#include "common.hpp"
#include "defines.hpp"
namespace fs::vfs
{

class inode
{
  private:
    flag_t info;

  protected:
    u64 file_size;
    u64 index;
    u64 link_count;
    u64 ref_count;
    super_block *su_block;

  public:
    inode() = default;
    virtual ~inode() = default;
    // create file in disk
    virtual void create(dentry *entry, nameidata *idata);

    virtual bool has_permission(flag_t pf);

    virtual void mkdir(dentry *entry);
    virtual void rmdir();

    virtual void rename(dentry *old_entry, dentry *new_entry);
    virtual void link(dentry *old_entry, dentry *new_entry);
    virtual bool unlink(dentry *entry);

    virtual u64 hash();

    void set_type(inode_type_t t) { info = (info & 0xFFFFFFFFFFFFFFF0) | (u64)t; }
    inode_type_t get_type() const { return (inode_type_t)(info & 0xF); }

    u64 get_size() const { return file_size; }

    void set_super_block(super_block *block) { su_block = block; }
    super_block *get_super_block() const { return su_block; }

    void set_index(u64 index) { this->index = index; }
    u64 get_index() const { return index; }
};
} // namespace fs::vfs
