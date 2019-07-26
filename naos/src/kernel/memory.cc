#include "kernel/memory.hpp"
#include "kernel/kernel.hpp"

namespace memory
{
void *base_ptr;
void *current_ptr;
u64 limit;
blocks global_blocks;
const u64 linear_addr_offset = 0xffff800000000000;
void *kernel_virtaddr_to_phyaddr(void *virt_addr) { return ((char *)virt_addr - linear_addr_offset); }
void *kernel_phyaddr_to_virtaddr(void *phy_addr) { return ((char *)phy_addr + linear_addr_offset); }

Unpaged_Text_Section void *unpaged_kernel_virtaddr_to_phyaddr(void *virt_addr)
{
    return ((char *)virt_addr - linear_addr_offset);
}
Unpaged_Text_Section void *unpaged_kernel_phyaddr_to_virtaddr(void *phy_addr)
{
    return ((char *)phy_addr + linear_addr_offset);
}

void *alloc(u64 size, u64 align)
{
    char *start = (char *)(((u64)current_ptr + align - 1) & ~(align - 1));
    current_ptr = start + size;
    return start;
}
void init(const kernel_start_args *args, u64 fix_memory_limit)
{
    limit = fix_memory_limit;
    base_ptr = (void *)(args->data_base + args->data_size);
    current_ptr = base_ptr;
}
void set_virtual()
{
    base_ptr = kernel_phyaddr_to_virtaddr(base_ptr);
    current_ptr = kernel_phyaddr_to_virtaddr(current_ptr);
}
} // namespace memory
