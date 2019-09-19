#pragma once
#include "common.hpp"
namespace fs::vfs
{
class super_block;

class file_system
{
  protected:
    const char *name;
    int version;
    super_block *su_block;

  public:
    file_system(const char *name, int version)
        : name(name)
        , version(version){};

    virtual ~file_system(){};

    const char *get_name() { return name; }
    super_block *get_super_block() { return su_block; };
    virtual bool load(const char *device_name, byte *data, u64 size) = 0;
    virtual void unload() = 0;
};
} // namespace fs::vfs
