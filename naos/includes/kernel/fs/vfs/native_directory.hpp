#pragma once

#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/kobject.hpp"

namespace fs::vfs
{

class native_directory final : public kobject
{
  public:
    native_directory(dentry *root, dentry *current)
        : kobject(type_e::directory)
        , root_(root)
        , current_(current)
    {
    }

    static type_e type_of() { return type_e::directory; }

    dentry *root() const { return root_; }
    dentry *current() const { return current_; }
    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    dentry *root_;
    dentry *current_;
};

} // namespace fs::vfs
