#pragma once
#include "common.hpp"
namespace fs::vfs
{
class dentry;
class file
{
  protected:
    i64 pointer_offset;
    flag_t mode;
    dentry *entry;
    u64 ref_count;

  public:
    file()
        : pointer_offset(0)
        , ref_count(0){};
    virtual ~file() = default;

    virtual int open(dentry *entry, flag_t mode) = 0;
    virtual int close() = 0;

    void add_ref() { ref_count++; }
    void remove_ref() { ref_count--; }

    virtual u64 read(byte *ptr, u64 max_size) = 0;
    virtual u64 write(byte *ptr, u64 size) = 0;

    virtual void flush() = 0;

    virtual void seek(i64 offset);
    virtual void move(i64 where);
    virtual i64 current_offset();

    u64 size() const;
    dentry *get_entry() const;
};
} // namespace fs::vfs
