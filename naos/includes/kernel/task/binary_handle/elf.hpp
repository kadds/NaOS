#pragma once
#include "bin_handle.hpp"
#include "kernel/common.hpp"
namespace naos::data_plane
{
class memory_object;
} // namespace naos::data_plane

namespace bin_handle
{
class elf_handle : public handle
{
  public:
    bool load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
              memory::vm::info_t *old_mm_info, execute_info *info) override;
};

} // namespace bin_handle
