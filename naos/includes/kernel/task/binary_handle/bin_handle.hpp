#pragma once
#include "../../types.hpp"
#include "kernel/common.hpp"
#include "kernel/handle.hpp"
#include "kernel/kobject.hpp"

namespace naos::data_plane
{
class memory_object;
} // namespace naos::data_plane

namespace memory::vm
{
class info_t;
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
    void *program_header = nullptr;
    u64 program_header_entry_size = 0;
    u64 program_header_count = 0;
    u64 base_address = 0;
    u64 hwcap = 0;
};
class handle
{
  public:
    /// Load an executable from an immutable MemoryObject and its backing
    /// capability.
    virtual bool load(byte *header, const handle_t<naos::data_plane::memory_object> &object,
                      const khandle &backing, memory::vm::info_t *new_mm_info, execute_info *info) = 0;
};

class bin_handle : public handle
{
  public:
    bool load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
              memory::vm::info_t *new_mm_info, execute_info *info) override;
};

void init();
void register_handle(handle *handle_class, const char *name);
bool unregister_handle(handle *handle_class, const char *name);

bool load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
          memory::vm::info_t *new_mm_info, execute_info *info);
} // namespace bin_handle
