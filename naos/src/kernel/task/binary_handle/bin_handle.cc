#include "kernel/task/binary_handle/bin_handle.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/str.hpp"
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

bool bin_handle::load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info)
{
    auto &vma = new_mm_info->vma;
    using namespace memory::vm;
    using namespace arch::task;
    auto new_vm = new_mm_info->map_file(memory::user_code_bottom_address, file, 0, file->size(), file->size(),
                                        memory::vm::flags::readable | memory::vm::flags::writeable |
                                            memory::vm::flags::executeable | memory::vm::flags::user_mode);
    info->entry_start_address = (void *)new_vm->start;

    auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                     memory::vm::flags::readable | memory::vm::flags::writeable |
                                         memory::vm::flags::expand | memory::vm::flags::user_mode,
                                     memory::vm::fill_expand_vm, 0);

    info->stack_top = (void *)stack_vm->end;
    info->stack_bottom = (void *)stack_vm->start;

    return true;
}

using array_t = util::array<handle_data>;

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
        if (it->handle_ptr == handle_class && util::strcmp(name, it->name) == 0)
        {
            handles->remove(it);
            return true;
        }
    }
    return false;
}

bool load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info)
{
    for (auto it = handles->begin(); it != handles->end(); ++it)
    {
        if (it->handle_ptr->load(header, file, new_mm_info, info))
        {
            info->user_data = (u64)it->handle_ptr;
            return true;
        }
    }
    return false;
}

bool load_bin(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info)
{
    bin_handle_ptr->load(header, file, new_mm_info, info);
    return true;
}

} // namespace bin_handle
