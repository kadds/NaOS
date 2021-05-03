#include "kernel/mm/memory.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/msg_queue.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/mm/zone.hpp"

namespace memory
{

VirtBootAllocator *VirtBootAllocatorV;
PhyBootAllocator *PhyBootAllocatorV;
KernelCommonAllocator *KernelCommonAllocatorV;
KernelVirtualAllocator *KernelVirtualAllocatorV;
MemoryAllocator *MemoryAllocatorV;
zones *KernelBuddyAllocatorV;

lock::spinlock_t buddy_lock;

phy_addr_t PhyBootAllocator::base_ptr;
phy_addr_t PhyBootAllocator::current_ptr;
bool PhyBootAllocator::available;

u64 max_memory_available;
u64 max_memory_maped;
u64 limit;
zones *global_zones;

vm::info_t *kernel_vm_info;

struct kmalloc_t
{
    u64 size;
    const char *name;
    slab_group *group;
    kmalloc_t(u64 size, const char *name)
        : size(size)
        , name(name)
        , group(nullptr){};
} kmalloc_fixed_slab_size[] = {
    {8, "kmalloc-8"},       {16, "kmalloc-16"},     {24, "kmalloc-24"},     {32, "kmalloc-32"},
    {40, "kmalloc-40"},     {48, "kmalloc-48"},     {56, "kmalloc-56"},     {64, "kmalloc-64"},
    {80, "kmalloc-80"},     {96, "kmalloc-96"},     {112, "kmalloc-112"},   {128, "kmalloc-128"},
    {160, "kmalloc-160"},   {192, "kmalloc-192"},   {256, "kmalloc-256"},   {384, "kmalloc-384"},
    {448, "kmalloc-448"},   {512, "kmalloc-512"},   {768, "kmalloc-768"},   {960, "kmalloc-960"},
    {1024, "kmalloc-1024"}, {1536, "kmalloc-1536"}, {2048, "kmalloc-2048"}, {3072, "kmalloc-3072"},
    {4096, "kmalloc-4096"}, {6144, "kmalloc-6144"}, {8192, "kmalloc-8192"}};

const char *get_type_str(map_type_t type)
{
    switch (type)
    {
        case map_type_t::available:
            return "available";
        case map_type_t::reserved:
            return "reserved";
        case map_type_t::acpi:
            return "acpi";
        case map_type_t::nvs:
            return "nvs";
        case map_type_t::badram:
            return "badram";
        default:
            return "unknown";
    }
}

int detect_zones(const kernel_start_args *args)
{
    kernel_memory_map_item *mm_item = pa2va<kernel_memory_map_item *>(phy_addr_t::from(args->mmap));
    max_memory_available = 0;
    max_memory_maped = 0;
    int zone_count = 0;

    for (u32 i = 0; i < args->mmap_count; i++, mm_item++)
    {
        trace::debug("Memory map index ", i, ": type:", get_type_str(mm_item->map_type),
                     ", range:", (void *)mm_item->addr, "-", (void *)((char *)mm_item->addr + mm_item->len),
                     ", size:", mm_item->len, "bytes -> ", mm_item->len >> 10, "Kib -> ", mm_item->len >> 20, "Mib");
        if (mm_item->map_type == map_type_t::available)
        {
            phy_addr_t start = align_up(phy_addr_t::from(mm_item->addr), page_size);
            phy_addr_t end = align_down(phy_addr_t::from(mm_item->addr + mm_item->len), page_size);
            if (likely(start >= end))
            {
                continue;
            }
            // kernel loaded at 0x10000 1Mib
            if (start <= phy_addr_t::from(100000))
            {
                continue;
            }
            zone_count++;
            i64 len = end - start;

            max_memory_available += len;
        }
        if (mm_item->addr + mm_item->len > max_memory_maped)
        {
            max_memory_maped = mm_item->addr + mm_item->len;
        }
    }
    trace::info("Memory available ", max_memory_available, "bytes -> ", max_memory_available >> 10, "Kib -> ",
                max_memory_available >> 20, "Mib -> ", max_memory_available >> 30, "Gib");
    // 16MB
    if (max_memory_available < 0x1000000)
    {
        trace::panic("Too few memory to boot");
    }

    return zone_count;
}

void init(const kernel_start_args *args, u64 fix_memory_limit)
{
    if (args->mmap_count == 0)
        trace::panic("memory map shouldn't empty");

    PhyBootAllocator::init(phy_addr_t::from(args->data_base + args->data_size));
    VirtBootAllocator vb;
    VirtBootAllocatorV = New<VirtBootAllocator>(&vb);
    PhyBootAllocatorV = New<PhyBootAllocator>(&vb);

    int zone_count = detect_zones(args);
    auto mm_item = pa2va<kernel_memory_map_item *>(phy_addr_t::from(args->mmap));

    global_zones = (memory::zones *)VirtBootAllocatorV->allocate(sizeof(zones) + zone_count * sizeof(zone), 8);
    global_zones = new (global_zones) memory::zones(zone_count);

    int zone_id = 0;
    for (u32 i = 0; i < args->mmap_count; i++, mm_item++)
    {
        if (mm_item->map_type != map_type_t::available)
        {

            continue;
        }

        phy_addr_t start = align_up(phy_addr_t::from(mm_item->addr), page_size);
        phy_addr_t end = align_down(phy_addr_t::from(mm_item->addr + mm_item->len), page_size);
        if (start >= end)
        {

            continue;
        }
        if (start <= phy_addr_t::from(100000))
        {

            continue;
        }

        zone *z = global_zones->at(zone_id);
        if (end > phy_addr_t::from(0x100000) && start <= phy_addr_t::from(0x100000))
        {
            z = new (z) zone(start, end, phy_addr_t::from(0x100000),
                             phy_addr_t::from(PhyBootAllocatorV->current_ptr_address()));
        }
        else
        {
            z = new (z) zone(start, end, nullptr, nullptr);
        }
        trace::debug("Memory zone index ", zone_id, " num of page ", z->pages_count(), ", block of buddy ",
                     z->buddy_blocks_count());
        zone_id++;
    }

    KernelCommonAllocatorV = New<KernelCommonAllocator>(VirtBootAllocatorV);
    KernelVirtualAllocatorV = New<KernelVirtualAllocator>(VirtBootAllocatorV);
    MemoryAllocatorV = New<MemoryAllocator>(VirtBootAllocatorV);
    // after kernel common allocator is initialized
    global_kmalloc_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);
    global_dma_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);
    global_object_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);

    for (auto &i : kmalloc_fixed_slab_size)
    {
        i.group = New<slab_group>(VirtBootAllocatorV, i.size, i.name, 8, 0);
    }

    // map the kernel and data
    phy_addr_t start_kernel = align_down(phy_addr_t::from(args->kernel_base), page_size);
    phy_addr_t end_kernel = align_up(phy_addr_t::from(args->kernel_base) + args->kernel_size, page_size);

    phy_addr_t start_data = align_down(phy_addr_t::from(args->data_base), page_size);
    phy_addr_t end_data = align_up(
        phy_addr_t::from(PhyBootAllocator::current_ptr_address()) + sizeof(vm::info_t) + fix_memory_limit, page_size);

    if (start_data < end_kernel)
    {
        start_data = end_kernel;
        trace::warning("Data space is in kernel code space");
        if (end_data < start_data)
        {
            end_data = start_data;
        }
    }

    phy_addr_t start_image_data = align_down(phy_addr_t::from(args->rfsimg_start), page_size);
    phy_addr_t end_image_data = align_up(phy_addr_t::from(args->rfsimg_start + args->rfsimg_size), page_size);

    if (end_image_data <= start_data || start_image_data >= end_data)
    {
        global_zones->tag_alloc(start_image_data, end_image_data);
    }
    // Don't use 0x0 - 0x100000 lower 1MB memory
    global_zones->tag_alloc(phy_addr_t::from(0x0), phy_addr_t::from(0x100000));
    // tell buddy system kernel used
    global_zones->tag_alloc(start_kernel, end_kernel);
    global_zones->tag_alloc(start_data, end_data);

    trace::debug("Kernel(code):", reinterpret_cast<addr_t>(start_kernel()), "-", reinterpret_cast<addr_t>(end_kernel()),
                 ", size:", (end_kernel - start_kernel), " -> ", (end_kernel - start_kernel) >> 10, "Kib");

    trace::debug("Kernel(boot data):", reinterpret_cast<addr_t>(start_data()), "-",
                 reinterpret_cast<addr_t>(end_data()), ", size:", (end_data - start_data), " -> ",
                 (end_data - start_data) >> 10, "Kib");

    trace::debug("Kernel(image data):", reinterpret_cast<addr_t>(start_image_data()), "-",
                 reinterpret_cast<addr_t>(end_image_data()), ", size:", (end_image_data - start_image_data), " -> ",
                 (end_image_data - start_image_data) >> 10, "Kib");

    KernelBuddyAllocatorV = global_zones;

    trace::debug("kmalloc prepared.");

    memory::vm::init();
    kernel_vm_info = New<vm::info_t>(VirtBootAllocatorV);
    kernel_vm_info->vma.set_range(memory::kernel_mmap_top_address, memory::kernel_mmap_bottom_address);

    PhyBootAllocatorV->discard();
    kassert(PhyBootAllocatorV->current_ptr_address() <= end_data(), "BootAllocator is Out of memory");
    msg_queue_init();
}

void listen_page_fault() { vm::listen_page_fault(); }

u64 get_max_available_memory() { return max_memory_available; }

u64 get_max_maped_memory() { return max_memory_maped; }

void *kmalloc(u64 size, u64 align)
{
    if (unlikely(align > size))
    {
        trace::panic("align is larger than size");
    }
    constexpr int tmp = sizeof(kmalloc_fixed_slab_size) / sizeof(kmalloc_fixed_slab_size[0]) - 1;
    if (unlikely(size > kmalloc_fixed_slab_size[tmp].size))
    {
        trace::panic("allocate size is too large");
    }

    u64 left = 0, right = sizeof(kmalloc_fixed_slab_size) / sizeof(kmalloc_fixed_slab_size[0]), mid;

    while (left < right)
    {
        mid = left + (right - left) / 2;
        if (kmalloc_fixed_slab_size[mid].size >= size)
        {
            if (kmalloc_fixed_slab_size[mid - 1].size < size)
            {
                break;
            }
            right = mid;
        }
        else
        {
            left = mid + 1;
        }
    }
    kassert(kmalloc_fixed_slab_size[mid].size >= size, "Kernel malloc check failed!");

    SlabObjectAllocator allocator(kmalloc_fixed_slab_size[mid].group);
    return allocator.allocate(size, align);
}

void kfree(void *addr)
{
    slab_group *s = slab_group::get_group_from(addr);
    if (s == nullptr)
    {
        trace::warning("address isn't malloc from kmalloc ", reinterpret_cast<addr_t>(addr));
        return;
    }
    SlabObjectAllocator allocator(s);
    allocator.deallocate(addr);
    return;
}

void *vmalloc(u64 size, u64 align)
{
    auto vm = kernel_vm_info->vma.allocate_map(size, align, 0, 0);
    for (u64 start = vm->start; start < vm->end; start += page_size)
    {
        auto phy = memory::va2pa(KernelBuddyAllocatorV->allocate(1, 0));
        arch::paging::map((arch::paging::base_paging_t *)kernel_vm_info->mmu_paging.get_base_page(), (void *)start, phy,
                          arch::paging::frame_size::size_4kb, 1,
                          arch::paging::flags::writable | arch::paging::flags::present);
    }
    arch::paging::reload();
    return (void *)vm->start;
}

void vfree(void *addr)
{
    auto vm = kernel_vm_info->vma.get_vm_area((u64)addr);
    if (vm)
    {
        /// TODO: free page memory
        kernel_vm_info->vma.deallocate_map(vm);
        arch::paging::unmap((arch::paging::base_paging_t *)kernel_vm_info->mmu_paging.get_base_page(),
                            (void *)vm->start, arch::paging::frame_size::size_4kb,
                            (vm->end - vm->start) / arch::paging::frame_size::size_4kb);
    }
}

void *malloc_page() { return KernelBuddyAllocatorV->allocate(1, 0); }

void free_page(void *addr) { KernelBuddyAllocatorV->deallocate(addr); }

void *KernelCommonAllocator::allocate(u64 size, u64 align) { return kmalloc(size, align); }

void KernelCommonAllocator::deallocate(void *p) { kfree(p); }

void *KernelVirtualAllocator::allocate(u64 size, u64 align) { return vmalloc(size, align); }

void KernelVirtualAllocator::deallocate(void *p) { vfree(p); }

void *MemoryAllocator::allocate(u64 size, u64 align)
{
    if (size > 4096)
    {
        return vmalloc(size, align);
    }
    return kmalloc(size, align);
}

void MemoryAllocator::deallocate(void *p)
{
    auto vm = kernel_vm_info->vma.get_vm_area((u64)p);
    if (!vm)
    {
        vfree(p);
        return;
    }
    kfree(p);
}

} // namespace memory
