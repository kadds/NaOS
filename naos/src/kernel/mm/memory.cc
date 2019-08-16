#include "kernel/mm/memory.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/slab.hpp"
namespace memory
{
VirtBootAllocator *VirtBootAllocatorV;
PhyBootAllocator *PhyBootAllocatorV;
KernelCommonAllocator *KernelCommonAllocatorV;
void *PhyBootAllocator::base_ptr;
void *PhyBootAllocator::current_ptr;
u64 limit;
zones_t global_zones;

const u64 linear_addr_offset = 0xffff800000000000;
const u64 page_size = 0x1000;
const struct
{
    u64 size;
    const char *name;
} kmalloc_fixed_slab_size[] = {
    {8, "kmalloc-8"},       {16, "kmalloc-16"},     {24, "kmalloc-24"},     {32, "kmalloc-32"},    {48, "kmalloc-48"},
    {64, "kmalloc-64"},     {96, "kmalloc-96"},     {128, "kmalloc-128"},   {256, "kmalloc-256"},  {512, "kmalloc-512"},
    {1024, "kmalloc-1024"}, {2048, "kmalloc-2048"}, {4096, "kmalloc-4096"}, {8192, "kmalloc-8192"}};
void tag_zone_buddy_memory(char *start_addr, char *end_addr)
{
    for (int i = 0; i < global_zones.count; i++)
    {
        auto &zone = global_zones.zones[i];
        if ((zone.props & zone_t::prop::present) != zone_t::prop::present)
            continue;
        u64 start = start_addr > (char *)zone.start ? (u64)start_addr : (u64)zone.start;
        u64 end = end_addr < (char *)zone.end ? (u64)end_addr : (u64)zone.end;
        if (start < end)
        {
            u64 start_offset = (start - (u64)zone.start) / page_size;
            u64 end_offset = (end - (u64)zone.start) / page_size;
            zone.tag_used(start_offset, end_offset);
        }
    }
}
void init(const kernel_start_args *args, u64 fix_memory_limit)
{
    PhyBootAllocator::init((void *)(args->data_base + args->data_size));
    VirtBootAllocator vb;
    VirtBootAllocatorV = New<VirtBootAllocator>(&vb);
    PhyBootAllocatorV = New<PhyBootAllocator>(&vb);

    global_zones.count = 0;
    kernel_memory_map_item *mm_item = kernel_phyaddr_to_virtaddr(args->get_mmap_ptr());

    for (u32 i = 0; i < args->mmap_count; i++, mm_item++)
    {
        if (mm_item->map_type == map_type_t::available)
        {
            u64 start = ((u64)mm_item->addr + page_size - 1) & ~(page_size - 1);
            u64 end = ((u64)mm_item->addr + mm_item->len - 1) & ~(page_size - 1);
            if (start < end)
                global_zones.count++;
        }
    }
    mm_item = kernel_phyaddr_to_virtaddr(args->get_mmap_ptr());

    global_zones.zones = NewArray<zone_t>(VirtBootAllocatorV, global_zones.count);
    int cid = 0;
    for (u32 i = 0; i < args->mmap_count; i++, mm_item++)
    {
        if (mm_item->map_type != map_type_t::available)
            continue;
        u64 start = ((u64)mm_item->addr + page_size - 1) & ~(page_size - 1);
        u64 end = ((u64)mm_item->addr + mm_item->len - 1) & ~(page_size - 1);
        if (start >= end)
            continue;
        auto &item = global_zones.zones[cid];
        item.start = (void *)start;
        item.end = (void *)end;
        item.page_count = (end - start) / page_size;
        auto buddys = New<buddy_contanier>(VirtBootAllocatorV);

        item.buddy_impl = buddys;
        auto count = item.page_count / buddy_max_page;
        auto rest = item.page_count % buddy_max_page;
        if (rest > 0)
            buddys->count = count + 1;
        else
            buddys->count = count;

        if (buddys->count > 0)
        {
            buddys->buddys = NewArray<buddy>(VirtBootAllocatorV, buddys->count, buddy_max_page);
            item.props = zone_t::prop::present;
            if (rest > 0)
                buddys->buddys[buddys->count - 1].tag_alloc(rest, buddy_max_page - rest);
        }
        else
        {
            item.props = 0;
        }
        cid++;
    }

    char *end_kernel = (char *)PhyBootAllocator::current_ptr_address() + (sizeof(slab_cache_pool) + sizeof(u64)) * 3 +
                       fix_memory_limit;
    char *start_kernel = (char *)base_phy_addr;
    end_kernel = (char *)(((u64)end_kernel + page_size - 1) & ~(page_size - 1));
    // Do not use 0x0 - 0x100000 lower 1MB memory
    tag_zone_buddy_memory(0x0, (char *)0x100000);
    // tell buddy system kernel used
    tag_zone_buddy_memory(start_kernel, end_kernel);
    // auto r = ((buddy_contanier *)global_zones.zones[1].buddy_impl)->buddys->gen_tree();

    global_kmalloc_slab_cache_pool = New<slab_cache_pool>(VirtBootAllocatorV);
    for (auto i : kmalloc_fixed_slab_size)
    {
        global_kmalloc_slab_cache_pool->create_new_slab_group(i.size, i.name, 8, 0);
    }
    KernelCommonAllocatorV = New<KernelCommonAllocator>(VirtBootAllocatorV);
}
void *kmalloc(u64 size, u64 align)
{
    if (align > 8)
    {
        size *= 2;
    }
    if (size > kmalloc_fixed_slab_size[sizeof(kmalloc_fixed_slab_size) / sizeof(kmalloc_fixed_slab_size[0]) - 1].size)
    {
        return nullptr;
    }
    SlabSizeAllocator allocator(global_kmalloc_slab_cache_pool);
    return allocator.allocate(size, align);
}
void kfree(void *addr)
{
    SlabSizeAllocator allocator(global_kmalloc_slab_cache_pool);
    allocator.deallocate(addr);
}
void zone_t::tag_used(u64 offset_start, u64 offset_end)
{
    auto buddys = (buddy_contanier *)buddy_impl;
    u64 s_buddy = offset_start / buddy_max_page;
    u64 e_buddy = offset_end / buddy_max_page;
    u64 s_buddy_rest = offset_start % buddy_max_page;
    u64 e_buddy_rest = offset_end % buddy_max_page;
    if (s_buddy == e_buddy)
    {
        buddys->buddys[s_buddy].tag_alloc(s_buddy_rest, e_buddy_rest - s_buddy_rest);
        return;
    }
    buddys->buddys[s_buddy].tag_alloc(s_buddy_rest, buddy_max_page - s_buddy_rest);
    for (u64 i = s_buddy + 1; i < e_buddy; i++)
    {
        buddys->buddys[i].tag_alloc(0, buddy_max_page);
    }
    buddys->buddys[e_buddy].tag_alloc(0, e_buddy_rest);
}

} // namespace memory
