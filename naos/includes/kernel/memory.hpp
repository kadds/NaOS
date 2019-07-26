#pragma once
#include "common.hpp"
#include <new>
#include <utility>
namespace memory
{
extern void *base_ptr;
extern void *current_ptr;
extern u64 limit;
enum class alloc_props
{
    kernel_paging,

};
void *alloc(u64 size, u64 align);
void init(const kernel_start_args *args, u64 fix_memory_limit);
void set_virtual();

template <typename T, int align = alignof(T), typename... Args> T *New(Args &&... args)
{
    T *ptr = (T *)alloc(sizeof(T), align);
    return new (ptr) T(std::forward<Args>(args)...);
}

template <typename T, int align = alignof(T), typename... Args> T *NewArray(u64 count, Args &&... args)
{
    T *ptr = (T *)alloc(sizeof(T) * count, align);
    for (u64 i = 0; i < count; i++)
    {
        new (ptr + i) T(std::forward<Args>(args)...);
    }
    return ptr;
}

class bit_set
{
  private:
    u8 *data;
    int element_count;

  public:
    bit_set(int element_count)
        : element_count(element_count)
    {
        int bytes = element_count;
        bytes = (bytes + sizeof(u8) - 1) & ~(sizeof(u8) - 1);
        data = (u8 *)alloc(bytes, 1);
    }
    u8 get_element(int index) { return (data[index / 8] >> (index % 8)) & 0x1; }
    void set_element(int index, u8 value) {}
    void clear_element() {}
    ~bit_set();
};

struct block
{
    void *start;
    u64 len;
    void *end;
    int prop;
    bit_set *bits;
    enum page_frame_type : u8
    {
        free = 0,
        used
    };
};
struct blocks
{
    block *blocks;
    int block_count;
};
extern blocks global_blocks;

void *kernel_virtaddr_to_phyaddr(void *virt_addr);
void *kernel_phyaddr_to_virtaddr(void *phy_addr);
void *unpaged_kernel_virtaddr_to_phyaddr(void *virt_addr);
void *unpaged_kernel_virtaddr_to_phyaddr(void *virt_addr);

extern const u64 linear_addr_offset;

} // namespace memory
