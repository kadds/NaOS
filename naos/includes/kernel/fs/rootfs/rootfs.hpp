#pragma once
#include "kernel/fs/ramfs/ramfs.hpp"

namespace fs::rootfs
{
class file_system : public ramfs::file_system
{
  public:
    file_system();
    vfs::super_block *load(const char *device_name, const byte *data, u64 size) override;
    void unload(vfs::super_block *su_block) override;
};

void init(byte *start_root_image, u64 length);

class super_block : public ramfs::super_block
{
  public:
    super_block(file_system *fs)
        : ramfs::super_block(0xFFFFFF, fs){};
};

} // namespace fs::rootfs
