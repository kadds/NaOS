#include "kernel/mm/vm.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/cpu.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"

namespace memory::vm
{
irq::request_result _ctx_interrupt_ page_fault_func(const void *regs, u64 extra_data, u64 user_data)
{
    auto *thread = cpu::current().get_task();
    if (thread != nullptr)
    {
        auto info = (info_t *)thread->process->mm_info;

        if (extra_data >= 0xFFFF800000000000)
        {
            /// FIXME: user space page fault at kernel vm area
            info = (info_t *)memory::kernel_vm_info;
        }

        auto vm = info->vma.get_vm_area(extra_data);
        if (vm != nullptr)
        {
            if (vm->handle != nullptr)
            {
                if (!vm->handle(extra_data, vm))
                {
                    return irq::request_result::no_handled;
                }
            }
            arch::paging::reload();
            return irq::request_result::ok;
        }
    }
    return irq::request_result::no_handled;
}
vm_allocator::list_node_cache_allocator_t *vm_allocator::allocator;

void init()
{
    vm_allocator::allocator = memory::New<vm_allocator::list_node_cache_allocator_t>(memory::VirtBootAllocatorV);
}

void listen_page_fault() { irq::register_request_func(arch::exception::vector::page_fault, page_fault_func, 0); }

template <typename _T> _T *new_page_table()
{
    static_assert(sizeof(_T) == memory::page_size, "type _T must be a page table");
    return memory::New<_T, memory::page_size>(memory::KernelBuddyAllocatorV);
}

template <typename _T> void delete_page_table(_T *addr)
{
    static_assert(sizeof(_T) == memory::page_size, "type _T must be a page table");
    memory::Delete<_T>(memory::KernelBuddyAllocatorV, addr);
}

int search_vma(const vm_t &vm, u64 p)
{
    if (p >= vm.end)
        return 1;
    else if (p < vm.start)
        return -1;
    return 0;
}

const vm_t *vm_allocator::allocate_map(u64 size, u64 flags, vm_page_fault_func func, u64 user_data)
{
    size = (size + memory::page_size - 1) & ~(memory::page_size - 1);

    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    u64 low_bound = range_bottom;

    if (!list.empty())
    {
        // allocate first fit
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            vm_t *vm = &it;
            if (vm->start - low_bound >= size)
            {
                return &list.insert(vm_t(low_bound, low_bound + size, flags, func, user_data));
            }
            low_bound = vm->end;
        }
    }
    if (range_top < low_bound + size)
    {
        return nullptr;
    }
    return &list.insert(vm_t(low_bound, low_bound + size, flags, func, user_data));
}

void vm_allocator::deallocate_map(const vm_t *vm)
{
    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    list.remove(*vm);
}

bool vm_allocator::deallocate_map(u64 p)
{
    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    auto it = list.for_each(search_vma, p);
    if (it != list.end())
    {
        list.remove(it);
        return true;
    }
    return false;
}

const vm_t *vm_allocator::add_map(u64 start, u64 end, u64 flags, vm_page_fault_func func, u64 user_data)
{
    kassert((char *)start < (char *)end, "parameter start must < parameter end");
    kassert((u64)start == ((u64)start & ~(memory::page_size - 1)), "parameter start must aligned");
    kassert((u64)end == ((u64)end & ~(memory::page_size - 1)), "parameter end must aligned");
    if (unlikely(start < range_bottom))
        return nullptr;
    if (unlikely(end > range_top))
        return nullptr;

    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);

    if (list.empty())
    {
        return &list.insert(vm_t(start, end, flags, func, user_data));
    }

    auto it = list.for_each_last(search_vma, start);
    kassert(it != list.end(), "vma assert failed");
    it++;
    if (it != list.end())
    {
        if (it->start < end)
        {
            return nullptr;
        }
    }
    return &list.insert(vm_t(start, end, flags, func, user_data));
}

const vm_t *vm_allocator::get_vm_area(u64 p)
{
    uctx::RawReadLockUninterruptibleContext ctx(list_lock);

    auto it = list.for_each(search_vma, p);
    if (it == list.end())
        return nullptr;
    return &it;
}

mmu_paging::mmu_paging() { base_paging_addr = new_page_table<arch::paging::base_paging_t>(); }

mmu_paging::~mmu_paging() { delete_page_table((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::load_paging() { arch::paging::load((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::map_area(const vm_t *vm)
{
    if (unlikely(vm == nullptr))
        return;
    u64 attr = 0;
    if (vm->flags & flags::writeable)
    {
        attr |= arch::paging::flags::writable;
    }
    if (vm->flags & flags::user_mode)
    {
        attr |= arch::paging::flags::user_mode;
    }

    for (char *start = (char *)vm->start; start < (char *)vm->end; start += arch::paging::frame_size::size_4kb)
    {
        void *phy_addr = memory::kernel_virtaddr_to_phyaddr(memory::malloc_page());

        arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, start, phy_addr,
                          arch::paging::frame_size::size_4kb, 1, attr);
    }
}

void mmu_paging::map_area_phy(const vm_t *vm, void *phy_address_start)
{
    if (unlikely(vm == nullptr))
        return;
    u64 attr = 0;
    if (vm->flags & flags::writeable)
    {
        attr |= arch::paging::flags::writable;
    }
    if (vm->flags & flags::user_mode)
    {
        attr |= arch::paging::flags::user_mode;
    }

    char *phy_addr = (char *)phy_address_start;
    for (byte *start = (byte *)vm->start; start < (byte *)vm->end;
         start += arch::paging::frame_size::size_4kb, phy_addr += arch::paging::frame_size::size_4kb)
    {
        arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, start, phy_addr,
                          arch::paging::frame_size::size_4kb, 1, attr);
    }
}

void mmu_paging::unmap_area(const vm_t *vm)
{
    if (unlikely(vm == nullptr))
        return;
    uctx::UninterruptibleContext uic;
    if (vm->flags & flags::expand)
    {
        u64 page_count = (vm->end - vm->start) / page_size;
        for (u64 i = 0; i < page_count; i++)
        {
            auto vir = (void *)(vm->start + i * page_size);
            void *phy;
            if (arch::paging::get_map_address((arch::paging::base_paging_t *)base_paging_addr, vir, &phy))
            {
                arch::paging::unmap((arch::paging::base_paging_t *)base_paging_addr, (void *)vir,
                                    arch::paging::frame_size::size_4kb, 1);
                memory::free_page(memory::kernel_phyaddr_to_virtaddr(phy));
            }
        }
    }
    else
    {
        u64 page_count = (vm->end - vm->start) / page_size;
        for (u64 i = 0; i < page_count; i++)
        {
            auto vir = (void *)(vm->start + i * page_size);
            void *phy;
            if (arch::paging::get_map_address((arch::paging::base_paging_t *)base_paging_addr, vir, &phy))
            {
                memory::free_page(memory::kernel_phyaddr_to_virtaddr(phy));
            }
            else
            {
                trace::panic("mmu_paging umap failed! This is not a expand vm_area.");
            }
        }
        arch::paging::unmap((arch::paging::base_paging_t *)base_paging_addr, (void *)vm->start,
                            arch::paging::frame_size::size_4kb, page_count);
    }
}

void *mmu_paging::get_page_addr() { return base_paging_addr; }

void mmu_paging::sync_kernel()
{
    uctx::UninterruptibleContext icu;
    arch::paging::sync_kernel_page_table(
        (arch::paging::base_paging_t *)base_paging_addr,
        (arch::paging::base_paging_t *)memory::kernel_vm_info->mmu_paging.base_paging_addr);
}

info_t::info_t()
    : vma(memory::user_mmap_top_address, memory::user_code_bottom_address)
    , head_vm(nullptr)
    , current_head_ptr(0)
{
}

info_t::~info_t()
{
    uctx::RawWriteLockUninterruptibleContext ctx(vma.get_lock());
    auto &list = vma.get_list();
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        mmu_paging.unmap_area(&it);
    }
}

bool head_expand_vm(u64 page_addr, const vm_t *item);

void info_t::init_brk(u64 start)
{
    head_vm = vma.add_map(start, start + memory::user_head_size,
                          memory::vm::flags::readable | memory::vm::flags::writeable | memory::vm::flags::expand |
                              memory::vm::flags::user_mode,
                          memory::vm::head_expand_vm, 0);
    if (likely(head_vm))
        current_head_ptr = head_vm->start;
}

bool info_t::set_brk(u64 ptr)
{
    ptr = (ptr + memory::page_size - 1) & ~(memory::page_size - 1);

    if (ptr > head_vm->end || ptr < head_vm->start)
    {
        return false;
    }

    if (ptr > current_head_ptr)
    {
        /// add page
        /// \see head_expand_vm
    }
    else
    {
        /// remove pages
        vm_t vm = *head_vm;
        vm.start = ptr;
        vm.end = current_head_ptr;
        mmu_paging.unmap_area(&vm);
    }
    current_head_ptr = ptr;
    return true;
}

bool info_t::set_brk_now(u64 ptr)
{
    ptr = (ptr + memory::page_size - 1) & ~(memory::page_size - 1);

    if (ptr > head_vm->end || ptr < head_vm->start)
    {
        return false;
    }

    if (ptr > current_head_ptr)
    {
        /// add page
        auto current_map = current_head_ptr;
        while (ptr < current_map)
        {
            byte *ptr = (byte *)memory::malloc_page();
            vm_t vm = *head_vm;
            vm.start = current_map;
            vm.end = current_map + memory::page_size;
            mmu_paging.map_area_phy(&vm, (void *)ptr);
        }
    }
    else
    {
        /// remove pages
        vm_t vm = *head_vm;
        vm.start = ptr;
        vm.end = current_head_ptr;
        mmu_paging.unmap_area(&vm);
    }
    current_head_ptr = ptr;
    return true;
}

u64 info_t::get_brk() { return current_head_ptr; }

bool head_expand_vm(u64 page_addr, const vm_t *item)
{
    auto info = (info_t *)task::current_process()->mm_info;
    if (info->get_brk() > page_addr)
    {
        vm_t vm = *item;
        vm.start = (page_addr) & ~(memory::page_size - 1);
        vm.end = vm.start + memory::page_size;
        byte *ptr = (byte *)memory::malloc_page();
        info->mmu_paging.map_area_phy(&vm, memory::kernel_virtaddr_to_phyaddr(ptr));
        return true;
    }
    return false;
}

bool fill_expand_vm(u64 page_addr, const vm_t *item)
{
    auto info = (info_t *)task::current_process()->mm_info;

    if (item->flags & flags::expand)
    {
        vm_t vm = *item;
        vm.start = (page_addr) & ~(memory::page_size - 1);
        vm.end = vm.start + memory::page_size;
        byte *ptr = (byte *)memory::malloc_page();
        info->mmu_paging.map_area_phy(&vm, memory::kernel_virtaddr_to_phyaddr(ptr));
        return true;
    }
    return false;
}

bool fill_file_vm(u64 page_addr, const vm_t *item)
{
    map_t *mt = (map_t *)item->user_data;
    u64 page_start = (page_addr) & ~(memory::page_size - 1);
    u64 off = page_start - item->start;
    mt->file->move(mt->offset + off);
    byte *ptr = (byte *)memory::malloc_page();
    u64 read_size = mt->length > memory::page_size ? memory::page_size : mt->length;
    auto ksize = mt->file->read(ptr, read_size, 0);
    util::memzero(ptr + ksize, memory::page_size - ksize);
    vm_t vm = *item;
    vm.start = page_start;
    vm.end = vm.start + memory::page_size;
    mt->vm_info->mmu_paging.map_area_phy(&vm, memory::kernel_virtaddr_to_phyaddr(ptr));

    if (mt->vm_info->mmu_paging.get_page_addr() != memory::kernel_vm_info->mmu_paging.get_page_addr())
    {
        if (page_addr >= memory::kernel_mmap_bottom_address && page_addr <= memory::kernel_mmap_top_address)
        {
            memory::kernel_vm_info->mmu_paging.map_area_phy(&vm, memory::kernel_virtaddr_to_phyaddr(ptr));
        }
    }
    return true;
}

const vm_t *info_t::map_file(u64 start, fs::vfs::file *file, u64 file_map_offset, u64 map_length, flag_t page_ext_attr)
{
    if (start >= 0xFFFF800000000000)
    {
        return nullptr;
    }

    u64 alen = (map_length + memory::page_size - 1) & ~(memory::page_size - 1);
    auto cflags = flags::lock | flags::user_mode | flags::expand;
    auto func = fill_expand_vm;
    u64 user_data = (u64)this;

    if (file)
    {
        cflags |= flags::file;
        func = fill_file_vm;
        user_data = (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, file, file_map_offset, map_length, this);
    }

    if (start == 0)
    {
        return vma.allocate_map(alen, cflags | page_ext_attr, func, user_data);
    }
    return vma.add_map(start, start + alen, cflags | page_ext_attr, func, user_data);
}

void info_t::sync_map_file(u64 addr) {}

bool info_t::umap_file(u64 addr)
{
    auto vm = vma.get_vm_area(addr);
    if (!vm)
        return false;
    if (vm->flags & flags::file)
    {
        map_t *mt = (map_t *)vm->user_data;
        memory::Delete<>(memory::KernelCommonAllocatorV, mt);
    }
    vma.deallocate_map(vm);
    mmu_paging.unmap_area(vm);
    arch::paging::reload();
    return true;
}
} // namespace memory::vm
