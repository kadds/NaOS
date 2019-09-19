#pragma once
#include "common.hpp"

namespace fs::vfs
{
// create file attribute,
namespace permission_flags
{
enum permission : flag_t
{
    read = 1,
    write = 2,
    exec = 4,

};
} // namespace permission_flags

struct nameidata;
class super_block;
class dentry;

enum class inode_type_t : u64
{
    file,
    directory,
    link,
    device,
    other,
};

class inode
{
  private:
    flag_t info;

  protected:
    u64 file_size;
    u64 index;
    super_block *su_block;

  public:
    inode() = default;
    virtual ~inode() = default;
    // create file in disk
    virtual void create(dentry *entry, nameidata *idata) = 0;
    // remove file in disk
    virtual void remove() = 0;

    virtual bool has_permission(flag_t pf) = 0;

    virtual void mkdir(dentry *entry) = 0;
    virtual void rename(dentry *old_entry, dentry *new_entry) = 0;
    virtual void rmdir() = 0;

    virtual u64 hash() = 0;

    void set_type(inode_type_t t) { info = (info & 0xFFFFFFFFFFFFFFF0) | (u64)t; }
    inode_type_t get_type() const { return (inode_type_t)(info & 0xF); }

    u64 get_size() const { return file_size; }

    void set_super_block(super_block *block) { su_block = block; }
    super_block *get_super_block() const { return su_block; }
};
} // namespace fs::vfs
