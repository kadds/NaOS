#pragma once
#include "bin_handle.hpp"
#include "common.hpp"
namespace fs::vfs
{
class file;
} // namespace fs::vfs

namespace bin_handle
{
class elf_handle : public handle
{
  public:
    bool load(byte *header, fs::vfs::file *file, memory::vm::info_t *old_mm_info, execute_info *info) override;
};

} // namespace bin_handle
