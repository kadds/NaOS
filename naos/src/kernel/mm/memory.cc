#include "kernel/mm/memory.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vm.hpp"
namespace memory
{

const u64 user_stack_size = 0x400000;
// maximum stack size: 256MB
const u64 user_stack_maximum_size = 0x10000000;

const u64 user_mmap_top_address = 0x00008000000000 - 0x10000000;

const u64 user_code_bottom_address = 0x400000;

const u64 kernel_mmap_top_address = 0xFFFF800d00000000;

const u64 kernel_mmap_bottom_address = 0xFFFF800b00000000;

const u64 kernel_vga_bottom_address = 0xFFFF800a00000000;

const u64 kernel_vga_top_address = 0xFFFF800a00FFFFFF;

// 16 kb stack size in X86_64 arch
const int kernel_stack_page_count = 4;
// 16 kb irq stack size
const int interrupt_stack_page_count = 4;
// 8 kb exception stack size
const int exception_stack_page_count = 4;
// 8 kb exception extra stack size
const int exception_ext_stack_page_count = 4;
// 16 kb soft irq stack size
const int soft_irq_stack_page_count = 4;

const u64 kernel_stack_size = kernel_stack_page_count * memory::page_size;
const u64 interrupt_stack_size = interrupt_stack_page_count * memory::page_size;
const u64 exception_stack_size = exception_stack_page_count * memory::page_size;
const u64 exception_ext_stack_size = exception_ext_stack_page_count * memory::page_size;
const u64 soft_irq_stack_size = soft_irq_stack_page_count * memory::page_size;
const u64 max_memory_support = 0x100000000000ul;

VirtBootAllocator *VirtBootAllocatorV;
PhyBootAllocator *PhyBootAllocatorV;
KernelCommonAllocator *KernelCommonAllocatorV;
KernelVirtualAllocator *KernelVirtualAllocatorV;
void *PhyBootAllocator::base_ptr;
void *PhyBootAllocator::current_ptr;
bool PhyBootAllocator::available;

u64 max_memory_available;
u64 max_memory_maped;
u64 limit;
zones_t global_zones;

const u64 linear_addr_offset = 0xffff800000000000;
const u64 page_size = 0x1000;

vm::vm_allocator *kernel_vma;

const struct
{
    u64 size;
    const char *name;
} kmalloc_fixed_slab_size[] = {
    {8, "kmalloc-8"},       {16, "kmalloc-16"},     {24, "kmalloc-24"},     {32, "kmalloc-32"},
    {48, "kmalloc-48"},     {56, "kmalloc-56"},     {64, "kmalloc-64"},     {80, "kmalloc-80"},
    {96, "kmalloc-96"},     {128, "kmalloc-128"},   {192, "kmalloc-192"},   {256, "kmalloc-256"},
    {384, "kmalloc-384"},   {448, "kmalloc-448"},   {512, "kmalloc-512"},   {768, "kmalloc-768"},
    {960, "kmalloc-960"},   {1024, "kmalloc-1024"}, {1536, "kmalloc-1536"}, {2048, "kmalloc-2048"},
    {3072, "kmalloc-3072"}, {4096, "kmalloc-4096"}, {6144, "kmalloc-6144"}, {8192, "kmalloc-8192"}};

void tag_zone_buddy_memory(void *start_addr, void *end_addr)
{
    for (int i = 0; i < global_zones.count; i++)
    {
        auto &zone = global_zones.zones[i];

        u64 start = (char *)start_addr > (char *)zone.start ? (u64)start_addr : (u64)zone.start;
        u64 end = (char *)end_addr < (char *)zone.end ? (u64)end_addr : (u64)zone.end;
        if (start < end)
        {
            u64 start_offset = (start - (u64)zone.start) / page_size;
            u64 end_offset = (end - (u64)zone.start) / page_size;
            zone.tag_used(start_offset, end_offset);
        }
    }
}

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
            return "unknown";
        default:
            return "unknown";
    }
}

void init(const kernel_start_args *args, u64 fix_memory_limit)
{
    PhyBootAllocator::init((void *)(args->data_base + args->data_size));
    VirtBootAllocator vb;
    VirtBootAllocatorV = New<VirtBootAllocator>(&vb);
    PhyBootAllocatorV = New<PhyBootAllocator>(&vb);
    if (args->mmap_count == 0)
        trace::panic("mmap info size shouldn't 0");
    global_zones.count = 0;
    kernel_memory_map_item *mm_item = (kernel_memory_map_item *)kernel_phyaddr_to_virtaddr(args->mmap);
    max_memory_available = 0;
    max_memory_maped = 0;

    for (u32 i = 0; i < args->mmap_count; i++, mm_item++)
    {
        trace::debug("mm info ", i, ": type:", get_type_str(mm_item->map_type), ", start:", (void *)mm_item->addr,
                     ", length:", mm_item->len, ", end:", (void *)((char *)mm_item->addr + mm_item->len));
        if (mm_item->map_type == map_type_t::available)
        {
            u64 start = ((u64)mm_item->addr + page_size - 1) & ~(page_size - 1);
            u64 end = ((u64)mm_item->addr + mm_item->len - 1) & ~(page_size - 1);
            if (start < end)
                global_zones.count++;

            max_memory_available += mm_item->len;
        }
        if (mm_item->addr + mm_item->len > max_memory_maped)
            max_memory_maped = mm_item->addr + mm_item->len;
    }
    trace::info("memory available ", max_memory_available, " -> ", max_memory_available >> 10, "KB -> ",
                max_memory_available >> 20, "MB -> ", max_memory_available >> 30, "GB");

    mm_item = (kernel_memory_map_item *)kernel_phyaddr_to_virtaddr(args->mmap);

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
        if (likely(rest > 0))
            buddys->count = count + 1;
        else
            buddys->count = count;
        trace::debug("zone ", cid, " page count:", item.page_count, ", buddy count:", buddys->count);
        if (likely(buddys->count > 0))
        {
            buddys->buddys = NewArray<buddy>(VirtBootAllocatorV, buddys->count, buddy_max_page);
            if (rest > 0)
                buddys->buddys[buddys->count - 1].tag_alloc(rest, buddy_max_page - rest);
        }
        else
        {
        }
        cid++;
    }
    // map the kernel and data
    auto start_kernel = args->kernel_base & ~(page_size - 1);
    auto end_kernel = (args->kernel_base + args->kernel_size + page_size - 1) & ~(page_size - 1);

    auto start_data = (args->data_base) & ~(page_size - 1);
    auto end_data = (u64)PhyBootAllocator::current_ptr_address() + sizeof(slab_cache_pool) * 3 +
                    sizeof(void *) * 2 * 5 + sizeof(vm::vm_allocator) + fix_memory_limit;
    end_data = (end_data + page_size - 1) & ~(page_size - 1);

    if (start_data < end_kernel)
    {
        start_data = end_kernel;
        trace::warning("Data space is in kernel code space.");
        if (end_data < start_data)
        {
            end_data = start_data;
        }
    }

    auto start_image_data = (args->rfsimg_start) & ~(page_size - 1);
    auto end_image_data = (args->rfsimg_start + args->rfsimg_size + page_size - 1) & ~(page_size - 1);
    if (start_image_data < end_kernel)
    {
        trace::panic("Image space is in kernel code space.");
    }
    if (end_image_data <= start_data || start_image_data >= end_data)
    {
        tag_zone_buddy_memory((void *)start_image_data, (void *)end_image_data);
    }
    // Don't use 0x0 - 0x100000 lower 1MB memory
    tag_zone_buddy_memory((void *)0x0, (void *)0x100000);
    // tell buddy system kernel used
    tag_zone_buddy_memory((void *)start_kernel, (void *)end_kernel);
    tag_zone_buddy_memory((void *)start_data, (void *)end_data);

    trace::debug("Kernel(code):", (void *)start_kernel, "-", (void *)end_kernel,
                 ", length:", (end_kernel - start_kernel), " -> ", (end_kernel - start_kernel) >> 10, "Kib");

    trace::debug("Kernel(boot data):", (void *)start_data, "-", (void *)end_data, ", length:", (end_data - start_data),
                 " -> ", (end_data - start_data) >> 10, "Kib");

    trace::debug("Kernel(image data):", (void *)start_image_data, "-", (void *)end_image_data,
                 ", length:", (end_image_data - start_image_data), " -> ", (end_image_data - start_image_data) >> 10,
                 "Kib");

    KernelBuddyAllocatorV = New<BuddyAllocator>(VirtBootAllocatorV);

    global_kmalloc_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);
    global_dma_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);
    global_object_slab_domain = New<slab_cache_pool>(VirtBootAllocatorV);

    for (auto i : kmalloc_fixed_slab_size)
    {
        global_kmalloc_slab_domain->create_new_slab_group(i.size, i.name, 8, 0);
    }
    KernelCommonAllocatorV = New<KernelCommonAllocator>(VirtBootAllocatorV);

    memory::vm::init();
    kernel_vma =
        New<vm::vm_allocator>(VirtBootAllocatorV, memory::kernel_mmap_top_address, memory::kernel_mmap_bottom_address);
    KernelVirtualAllocatorV = New<KernelVirtualAllocator>(VirtBootAllocatorV);

    PhyBootAllocatorV->discard();
    kassert((u64)PhyBootAllocatorV->current_ptr_address() <= end_data, "BootAllocator is Out of memory");
}

irq::request_result _ctx_interrupt_ page_fault_func(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data)
{
    if (arch::cpu::current().is_in_user_context((void *)regs->rsp))
        return irq::request_result::no_handled;
    auto vm = kernel_vma->get_vm_area(extra_data);
    if (vm != nullptr)
    {
        arch::paging::copy_page_table(arch::paging::current(), arch::paging::get_kernel_paging());
        arch::paging::reload();
        return irq::request_result::ok;
    }
    return irq::request_result::no_handled;
}

void listen_page_fault()
{
    irq::insert_request_func(arch::exception::vector::page_fault, page_fault_func, 0);
    vm::listen_page_fault();
}

u64 get_max_available_memory() { return max_memory_available; }

u64 get_max_maped_memory() { return max_memory_maped; }

void *kmalloc(u64 size, u64 align)
{
    if (unlikely(align > size))
    {
        trace::panic("align is larger than size");
    }

    if (size > kmalloc_fixed_slab_size[sizeof(kmalloc_fixed_slab_size) / sizeof(kmalloc_fixed_slab_size[0]) - 1].size)
    {
        trace::panic("allocate size is too large");
        return nullptr;
    }
    SlabSizeAllocator allocator(global_kmalloc_slab_domain);
    return allocator.allocate(size, align);
}

void kfree(void *addr)
{
    SlabSizeAllocator allocator(global_kmalloc_slab_domain);
    allocator.deallocate(addr);
}

void *vmalloc(u64 size, u64 align)
{
    auto vm = kernel_vma->allocate_map(size, align);
    for (u64 start = vm->start; start < vm->end; start += page_size)
    {
        auto phy = memory::kernel_virtaddr_to_phyaddr(KernelBuddyAllocatorV->allocate(1, 0));
        arch::paging::map(arch::paging::get_kernel_paging(), (void *)start, phy, arch::paging::frame_size::size_4kb, 1,
                          arch::paging::flags::writable | arch::paging::flags::present);
    }
    if (arch::paging::current() == arch::paging::get_kernel_paging())
    {
        arch::paging::reload();
    }
    return (void *)vm->start;
}

void vfree(void *addr)
{
    auto vm = kernel_vma->deallocate_map(addr);
    if (vm.start != 0)
    {
        arch::paging::unmap(arch::paging::get_kernel_paging(), (void *)vm.start, arch::paging::frame_size::size_4kb,
                            (vm.end - vm.start) / arch::paging::frame_size::size_4kb);
    }
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
