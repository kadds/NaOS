#pragma once
#include "common.hpp"
#include "defines.hpp"
#include "pseudo.hpp"
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

    virtual int open(dentry *entry, flag_t mode);
    virtual int close();

    void add_ref() { ref_count++; }
    void remove_ref() { ref_count--; }

    u64 read(byte *ptr, u64 max_size, flag_t flags);
    u64 write(const byte *ptr, u64 size, flag_t flags);

    virtual void flush() = 0;

    virtual void seek(i64 offset);
    virtual void move(i64 where);
    virtual i64 current_offset();

    u64 size() const;
    dentry *get_entry() const;

    pseudo_t *get_pseudo();

  protected:
    virtual u64 iread(byte *ptr, u64 max_size, flag_t flags) = 0;
    virtual u64 iwrite(const byte *ptr, u64 size, flag_t flags) = 0;
};
} // namespace fs::vfs
