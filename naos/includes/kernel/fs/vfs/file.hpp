#pragma once
#include "common.hpp"
#include "defines.hpp"
#include "kernel/kobject.hpp"
#include "pseudo.hpp"
namespace fs::vfs
{
class dentry;
class file : public kobject
{
  protected:
    i64 offset;
    flag_t mode;
    dentry *entry;

  public:
    file()
        : kobject(type_e::file)
        , offset(0)
        , mode(0)
        , entry(nullptr)
    {
    }

    file(const file &f) = delete;
    file &operator=(const file &f) = delete;

    virtual ~file();

    virtual int open(dentry *entry, flag_t mode);

    i64 read(byte *ptr, u64 max_size, flag_t flags);
    i64 write(const byte *ptr, u64 size, flag_t flags);

    i64 pread(i64 offset, byte *ptr, u64 max_size, flag_t flags);
    i64 pwrite(i64 offset, const byte *ptr, u64 size, flag_t flags);

    virtual void flush() = 0;

    virtual void seek(i64 offset);
    virtual void move(i64 where);
    virtual i64 current_offset();

    u64 size() const;
    dentry *get_entry() const;

    pseudo_t *get_pseudo();
    flag_t get_mode() const { return mode; }

    static type_e type_of() { return type_e::file; }

  protected:
    virtual i64 iread(i64 &offset, byte *ptr, u64 max_size, flag_t flags) = 0;
    virtual i64 iwrite(i64 &offset, const byte *ptr, u64 size, flag_t flags) = 0;
};
} // namespace fs::vfs
