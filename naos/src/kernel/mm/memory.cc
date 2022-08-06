#include "kernel/mm/memory.hpp"
#include "common.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/msg_queue.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/trace.hpp"

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

struct memory_range
{
    phy_addr_t beg;
    phy_addr_t end;
    void set(phy_addr_t beg, phy_addr_t end)
    {
        this->beg = beg;
        this->end = end;
    }
    bool operator<(const memory_range &rhs) const { return beg < rhs.beg; }
    bool operator<=(const memory_range &rhs) const { return beg <= rhs.beg; }
    bool operator==(const memory_range &rhs) const { return beg == rhs.beg; }
    bool operator>(const memory_range &rhs) const { return beg > rhs.beg; }
    bool operator>=(const memory_range &rhs) const { return beg >= rhs.beg; }
};

int detect_zones(const kernel_start_args *args, memory_range used, memory_range *zones_range, int max_zones_range_count)
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
                // Don't use 0x0 - 0x100000 lower 1MB memory
                continue;
            }
            // split it
            if (used.end <= end)
            {
                start = used.end > start ? used.end : start;
            }
            start = align_up(start, large_page_size);
            if (zone_count + 1 > max_zones_range_count)
            {
                break;
            }
            i64 len = end - start;

            max_memory_available += len;
            zones_range[zone_count].set(start, end);
            zone_count++;
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

void init(kernel_start_args *args, u64 fix_memory_limit)
{
    if (args->mmap_count == 0)
        trace::panic("memory map shouldn't empty");

    PhyBootAllocator::init(phy_addr_t::from(args->data_base + args->data_size));
    VirtBootAllocator vb;
    VirtBootAllocatorV = New<VirtBootAllocator>(&vb);
    PhyBootAllocatorV = New<PhyBootAllocator>(&vb);

    phy_addr_t start_data = align_down(phy_addr_t::from(args->data_base), page_size);
    phy_addr_t end_data = phy_addr_t::from(PhyBootAllocator::current_ptr_address());

    constexpr int max_memory_range_support = 32;
    memory_range result_range[max_memory_range_support];
    memory_range used_range;

    // copy pointers
    const char *cmdline = reinterpret_cast<const char *>(args->command_line);
    u64 cmdlen = ::strlen(cmdline) + 1;
    auto cmdline_ptr = reinterpret_cast<byte *>(vb.allocate(cmdlen, 1));
    memcpy(cmdline_ptr, cmdline, cmdlen);
    args->command_line = reinterpret_cast<u64>(va2pa(cmdline_ptr).get());

    const char *bootloader = reinterpret_cast<const char *>(args->boot_loader_name);
    u64 bootloaderlen = ::strlen(bootloader) + 1;
    auto bootloader_ptr = reinterpret_cast<byte *>(vb.allocate(bootloaderlen, 1));
    memcpy(bootloader_ptr, bootloader, bootloaderlen);
    args->boot_loader_name = reinterpret_cast<u64>(va2pa(bootloader_ptr).get());

    u64 image_size = args->rfsimg_size;
    auto image_ptr = reinterpret_cast<byte *>(vb.allocate(image_size, 8));
    auto image_phy_addr = va2pa(image_ptr);
    memcpy(image_ptr, pa2va(phy_addr_t::from(args->rfsimg_start)), image_size);
    args->rfsimg_start = reinterpret_cast<u64>(image_phy_addr.get());

    auto end_used_memory_addr = align_up(image_ptr + image_size + page_size * 3 + fix_memory_limit, page_size);

    // map the kernel and data
    phy_addr_t start_kernel = align_down(phy_addr_t::from(args->kernel_base), page_size);
    phy_addr_t end_kernel = align_up(phy_addr_t::from(args->kernel_base) + args->kernel_size, page_size);

    if (start_data < end_kernel)
    {
        start_data = end_kernel;
        trace::warning("Data space is in kernel code space");
        if (end_data < start_data)
        {
            end_data = start_data;
        }
    }

    used_range.set(start_kernel, va2pa(end_used_memory_addr));

    int zone_count = detect_zones(args, used_range, result_range, max_memory_range_support);

    global_zones = (memory::zones *)VirtBootAllocatorV->allocate(sizeof(zones) + zone_count * sizeof(zone), 8);
    global_zones = new (global_zones) memory::zones(zone_count);

    for (int zone_id = 0; zone_id < zone_count; zone_id++)
    {
        auto range = result_range[zone_id];

        zone *z = global_zones->at(zone_id);
        z = new (z) zone(range.beg, range.end, nullptr, nullptr);
        trace::debug("Memory zone index ", zone_id, ", ", trace::hex(range.beg.get()), "-", trace::hex(range.end.get()),
                     " num of page ", z->pages_count(), ", block of buddy ", z->buddy_blocks_count());
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

    trace::debug("Kernel(code):", trace::hex(start_kernel()), "-", trace::hex(end_kernel()),
                 ", size:", (end_kernel - start_kernel), " -> ", (end_kernel - start_kernel) >> 10, "Kib");

    trace::debug("Kernel(boot data):", trace::hex(start_data()), "-", trace::hex(end_data()),
                 ", size:", (end_data - start_data), " -> ", (end_data - start_data) >> 10, "Kib");

    trace::debug("Kernel(image data):", trace::hex(image_phy_addr.get()), "-",
                 trace::hex((image_phy_addr + args->rfsimg_size).get()), ", size:", image_size, " -> ",
                 (image_size) >> 10, "Kib");

    KernelBuddyAllocatorV = global_zones;

    trace::debug("kmalloc prepared.");

    memory::vm::init();
    kernel_vm_info = New<vm::info_t>(VirtBootAllocatorV);
    kernel_vm_info->vma.set_range(memory::kernel_mmap_top_address, memory::kernel_mmap_bottom_address);

    PhyBootAllocatorV->discard();
    kassert(VirtBootAllocatorV->current_ptr_address() <= end_used_memory_addr, "BootAllocator is Out of memory");
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

    u64 left = 0, right = sizeof(kmalloc_fixed_slab_size) / sizeof(kmalloc_fixed_slab_size[0]), mid = 0;

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
    auto ptr = allocator.allocate(size, align);
    kassert((reinterpret_cast<u64>(ptr) & (align - 1)) == 0, "check alignment fail");
    return ptr;
}

void kfree(void *addr)
{
    slab_group *s = slab_group::get_group_from(addr);
    if (s == nullptr)
    {
        trace::panic("address isn't malloc from kmalloc ", trace::hex(addr));
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
                          arch::paging::flags::writable | arch::paging::flags::present, false);
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

void *KernelCommonAllocator::allocate(u64 size, u64 align) noexcept { return kmalloc(size, align); }

void KernelCommonAllocator::deallocate(void *p) noexcept { kfree(p); }

void *KernelVirtualAllocator::allocate(u64 size, u64 align) noexcept { return vmalloc(size, align); }

void KernelVirtualAllocator::deallocate(void *p) noexcept { vfree(p); }

void *MemoryAllocator::allocate(u64 size, u64 align) noexcept
{
    if (size > 4096)
    {
        return vmalloc(size, align);
    }
    return kmalloc(size, align);
}

void MemoryAllocator::deallocate(void *p) noexcept
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
