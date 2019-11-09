#pragma once
#include "../../types.hpp"
#include "common.hpp"
namespace fs::vfs
{
class file;
} // namespace fs::vfs

namespace memory::vm
{
struct info_t;

} // namespace memory::vm

namespace bin_handle
{
struct execute_info
{
    void *entry_start_address;
    void *stack_top;
    void *stack_bottom;
    u64 err_code;
    u64 user_data;
};
class handle
{
  public:
    virtual bool load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info) = 0;
};

class bin_handle : public handle
{
  public:
    bool load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info) override;
};

void init();
void register_handle(handle *handle_class, const char *name);
bool unregister_handle(handle *handle_class, const char *name);

bool load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info);
bool load_bin(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info);

} // namespace bin_handle
