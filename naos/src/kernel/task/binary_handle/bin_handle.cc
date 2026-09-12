#include "kernel/task/binary_handle/bin_handle.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/task/binary_handle/elf.hpp"

namespace bin_handle
{
struct handle_data
{
    handle *handle_ptr;
    const char *name;
    handle_data(handle *h, const char *name)
        : handle_ptr(h)
        , name(name)
    {
    }
};

bool bin_handle::load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
                      memory::vm::info_t *new_mm_info, execute_info *info)
{
    (void)header;
    (void)object;
    (void)backing;
    (void)new_mm_info;
    (void)info;
    return false;
}

using array_t = freelibcxx::vector<handle_data>;

array_t *handles;
bin_handle *bin_handle_ptr;

void init()
{
    handles = memory::New<array_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    register_handle(memory::New<elf_handle>(memory::KernelCommonAllocatorV), "elf");
    bin_handle_ptr = memory::New<bin_handle>(memory::KernelCommonAllocatorV);
}

void register_handle(handle *handle_class, const char *name) { handles->push_back(handle_data(handle_class, name)); }

bool unregister_handle(handle *handle_class, const char *name)
{
    for (auto it = handles->begin(); it != handles->end(); ++it)
    {
        if (it->handle_ptr == handle_class && strcmp(name, it->name) == 0)
        {
            handles->remove(it);
            return true;
        }
    }
    return false;
}

bool load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
          memory::vm::info_t *new_mm_info, execute_info *info)
{
    for (auto it = handles->begin(); it != handles->end(); ++it)
    {
        if (it->handle_ptr->load(header, object, backing, new_mm_info, info))
        {
            info->user_data = (u64)it->handle_ptr;
            return true;
        }
    }
    return false;
}

} // namespace bin_handle
