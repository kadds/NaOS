#include "kernel/mm/vm.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/irq.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace memory::vm
{
irq::request_result _ctx_interrupt_ page_fault_func(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data)
{
    auto *thread = arch::cpu::current().get_task();
    if (thread != nullptr)
    {
        auto info = (info_t *)thread->process->mm_info;

        if (extra_data >= 0xFFFF800000000000)
        {
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

void listen_page_fault() { irq::insert_request_func(arch::exception::vector::page_fault, page_fault_func, 0); }

template <typename _T> _T *new_page_table()
{
    static_assert(sizeof(_T) == 0x1000, "type _T must be a page table");
    return memory::New<_T, 0x1000>(memory::KernelBuddyAllocatorV);
}

template <typename _T> void delete_page_table(_T *addr)
{
    static_assert(sizeof(_T) == 0x1000, "type _T must be a page table");
    memory::Delete<_T>(memory::KernelBuddyAllocatorV, addr);
}

const vm_t *vm_allocator::allocate_map(u64 size, u64 flags, vm_page_fault_func func, u64 user_data)
{
    size = (size + memory::page_size - 1) & ~(memory::page_size - 1);

    uctx::SpinLockUnInterruptableContext ctx(list_lock);

    if (!list.empty())
    {
        u64 last_end = range_top;

        for (auto it = list.rbegin(); it != list.rend(); --it)
        {
            vm_t *vm = &it;
            if (last_end - vm->start >= size)
            {
                return &list.insert(it, vm_t(vm->end, vm->end + size, flags, func, user_data));
            }
            last_end = vm->end;
        }
    }
    else
    {
        if (range_top < range_bottom + size)
        {
            return nullptr;
        }
        return &list.push_back(vm_t(range_top - size, range_top, flags, func, user_data));
    }
    return nullptr;
}

void vm_allocator::deallocate_map(const vm_t *vm)
{
    uctx::SpinLockUnInterruptableContext ctx(list_lock);
    list.remove(list.find(*vm));
}

bool vm_allocator::deallocate_map(u64 p)
{
    uctx::SpinLockUnInterruptableContext ctx(list_lock);
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->start <= p || it->end > p)
        {
            list.remove(it);
            return true;
        }
    }
    return false;
}

const vm_t *vm_allocator::add_map(u64 start, u64 end, u64 flags, vm_page_fault_func func, u64 user_data)
{
    kassert((char *)start < (char *)end, "parameter start must < parameter end");
    kassert((u64)start == ((u64)start & ~(memory::page_size - 1)), "parameter start must aligned");
    kassert((u64)end == ((u64)end & ~(memory::page_size - 1)), "parameter end must aligned");
    uctx::SpinLockUnInterruptableContext ctx(list_lock);
    if (unlikely(start < range_bottom))
        return nullptr;
    if (unlikely(end > range_top))
        return nullptr;

    if (list.begin() == list.end())
    {
        return &list.push_back(vm_t(start, end, flags, func, user_data));
    }

    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if ((char *)start < (char *)it->start)
        {
            if ((char *)end <= (char *)it->start)
            {
                return &list.insert(it, vm_t(start, end, flags, func, user_data));
            }
            return nullptr;
        }
    }
    return &list.push_back(vm_t(start, end, flags, func, user_data));
}

const vm_t *vm_allocator::get_vm_area(u64 p)
{
    uctx::SpinLockUnInterruptableContext ctx(list_lock);
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if ((char *)it->start <= (char *)p)
        {
            if ((char *)it->end > (char *)p)
            {
                return &it;
            }
        }
    }
    return nullptr;
}

mmu_paging::mmu_paging() { base_paging_addr = new_page_table<arch::paging::base_paging_t>(); }

mmu_paging::~mmu_paging() { delete_page_table((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::load_paging() { arch::paging::load((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::expand_area(const vm_t *vm, void *ptr)
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

    byte *start = (byte *)(((u64)ptr) & ~(memory::page_size - 1));

    void *phy_addr = memory::kernel_virtaddr_to_phyaddr(memory::KernelBuddyAllocatorV->allocate(1, 0));

    arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, start, phy_addr,
                      arch::paging::frame_size::size_4kb, 1, attr);
}

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
        void *phy_addr = memory::kernel_virtaddr_to_phyaddr(memory::KernelBuddyAllocatorV->allocate(1, 0));

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

    arch::paging::unmap((arch::paging::base_paging_t *)base_paging_addr, (void *)vm->start,
                        arch::paging::frame_size::size_4kb, (vm->end - vm->start) / arch::paging::frame_size::size_4kb);
}

void *mmu_paging::get_page_addr() { return base_paging_addr; }

void mmu_paging::sync_kernel()
{
    arch::paging::sync_kernel_page_table(
        (arch::paging::base_paging_t *)base_paging_addr,
        (arch::paging::base_paging_t *)memory::kernel_vm_info->mmu_paging.base_paging_addr);
}

info_t::info_t()
    : vma(memory::user_mmap_top_address, memory::user_code_bottom_address)
{
}

bool fill_expand_vm(u64 page_addr, const vm_t *item)
{
    auto info = (info_t *)task::current_process()->mm_info;

    if (item->flags & flags::expand)
    {
        vm_t vm = *item;
        vm.start = (page_addr) & ~(memory::page_size - 1);
        vm.end = vm.start + memory::page_size;
        byte *ptr = (byte *)memory::KernelBuddyAllocatorV->allocate(1, 0);
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
    byte *ptr = (byte *)memory::KernelBuddyAllocatorV->allocate(1, 0);
    u64 read_size = mt->length > memory::page_size ? memory::page_size : mt->length;
    auto ksize = mt->file->read(ptr, read_size);
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

const vm_t *map_file(u64 start, info_t *vm_info, fs::vfs::file *file, u64 file_map_offset, u64 map_length,
                     flag_t page_ext_attr)
{
    if (start >= 0xFFFF800000000000)
    {
        return nullptr;
    }

    u64 alen = (map_length + memory::page_size - 1) & ~(memory::page_size - 1);

    if (start == 0)
    {
        auto vm = vm_info->vma.allocate_map(
            alen, flags::file | flags::lock | memory::vm::flags::user_mode | page_ext_attr, fill_file_vm,
            (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, file, file_map_offset, map_length, vm_info));

        return vm;
    }
    auto vm = vm_info->vma.add_map(
        start, start + alen, flags::file | flags::lock | memory::vm::flags::user_mode | page_ext_attr, fill_file_vm,
        (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, file, file_map_offset, map_length, vm_info));

    return vm;
}

const vm_t *map_shared_file(const vm_t *shared_vm, info_t *vm_info, flag_t ext_flags)
{
    auto alen = shared_vm->end - shared_vm->start;
    map_t *mt = (map_t *)shared_vm->user_data;

    auto vm = vm_info->vma.allocate_map(
        alen, flags::file | flags::lock | ext_flags, fill_file_vm,
        (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, mt->file, mt->offset, mt->length, vm_info));
    return vm;
}

const vm_t *map_shared_file(u64 start, const vm_t *shared_vm, info_t *vm_info, flag_t ext_flags)
{
    auto alen = shared_vm->end - shared_vm->start;
    map_t *mt = (map_t *)shared_vm->user_data;

    auto vm = vm_info->vma.add_map(
        start, start + alen, flags::file | flags::lock | ext_flags, fill_file_vm,
        (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, mt->file, mt->offset, mt->length, vm_info));
    return vm;
}

void sync_map_file(const vm_t *vm) {}

void umap_file(const vm_t *vm)
{
    map_t *mt = (map_t *)vm->user_data;
    mt->vm_info->vma.deallocate_map(vm);
}

} // namespace memory::vm
