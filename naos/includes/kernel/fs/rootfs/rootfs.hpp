#pragma once
#include "kernel/fs/ramfs/ramfs.hpp"

namespace fs::rootfs
{

void init(byte *start_root_image, u64 length);

class super_block : public ramfs::super_block
{
  public:
    super_block()
        : ramfs::super_block(0xFFFFFF){};
};

class file_system : public ramfs::file_system
{
  public:
    file_system();
    bool load(const char *device_name, byte *data, u64 size) override;
    void unload() override;
};

} // namespace fs::rootfs
