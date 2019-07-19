#include "kernel/memory.hpp"
namespace memory
{
void *base_ptr;
void *current_ptr;
u64 limit;

void *alloc(u64 size, u64 align)
{
    char *start = (char *)(((u64)current_ptr + align - 1) & ~(align - 1));
    current_ptr = start + size;
    return start;
}
void init(u64 data_base, u64 data_limit)
{
    limit = data_limit;
    base_ptr = (void *)data_base;
    current_ptr = base_ptr;
}

} // namespace memory
