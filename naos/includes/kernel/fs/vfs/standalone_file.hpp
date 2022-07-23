#pragma once
#include "common.hpp"
#include "defines.hpp"
#include "file.hpp"
#include "pseudo.hpp"
namespace fs::vfs
{
class dentry;
class standalone_file : public file
{
  public:
    standalone_file();

    standalone_file(const standalone_file &f) = delete;
    file &operator=(const standalone_file &f) = delete;

    virtual ~standalone_file(){};

    void flush() override;

  protected:
    i64 iread(i64 &offset, byte *ptr, u64 max_size, flag_t flags) override;
    i64 iwrite(i64 &offset, const byte *ptr, u64 size, flag_t flags) override;
};
} // namespace fs::vfs
