#pragma once
#include "common.hpp"
namespace fs::vfs
{
class super_block;

class file_system
{
  protected:
    const char *name;

  public:
    file_system(const char *name)
        : name(name){};

    virtual ~file_system(){};

    const char *get_name() { return name; }
    virtual super_block *load(const char *device_name, byte *data, u64 size) = 0;
    virtual void unload(super_block *su_block) = 0;
};
} // namespace fs::vfs
