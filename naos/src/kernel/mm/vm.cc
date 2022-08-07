#include "kernel/mm/vm.hpp"
#include "common.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/cpu.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/signal.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"

namespace memory::vm
{

irq::request_result _ctx_interrupt_ page_fault_cow(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    auto *thread = cpu::current().get_task();
    if (thread != nullptr)
    {
        auto info = (info_t *)thread->process->mm_info;

        if (is_kernel_space_pointer(extra_data))
        {
            if (!inter->kernel_space)
            {
                trace::warning("process ", thread->process->pid, " using kernel space pointer ",
                               trace::hex(extra_data));
                auto &pack = thread->process->signal_pack;
                pack.send(thread->process, ::task::signal::sigstkflt, extra_data, 0, 0);
                return irq::request_result::ok;
            }
            else
            {
                trace::panic("kernel space cow");
            }
        }
        if (info->copy_at(extra_data))
        {
            arch::paging::reload();
            return irq::request_result::ok;
        }
    }
    return irq::request_result::no_handled;
}

irq::request_result _ctx_interrupt_ page_fault_present(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    auto *thread = cpu::current().get_task();
    if (thread != nullptr)
    {
        if (extra_data == 0)
        {
            trace::warning("null pointer access pid ", thread->process->pid, " tid ", thread->tid);
        }
        auto info = (info_t *)thread->process->mm_info;

        if (is_kernel_space_pointer(extra_data))
        {
            if (!inter->kernel_space)
            {
                trace::warning("process ", thread->process->pid, " using kernel space pointer ",
                               trace::hex(extra_data));
                auto &pack = thread->process->signal_pack;
                pack.send(thread->process, ::task::signal::sigsegv, extra_data, 0, 0);
                return irq::request_result::ok;
            }
            info = (info_t *)memory::kernel_vm_info;
        }

        auto vm = info->vma.get_vm_area(extra_data);
        if (vm != nullptr)
        {
            if (vm->handle != nullptr)
            {
                if (!vm->handle(info->vma, extra_data, vm))
                {
                    return irq::request_result::no_handled;
                }
            }
            arch::paging::reload();
            return irq::request_result::ok;
        }
        else
        {
            trace::info("vm area not found ", trace::hex(extra_data));
            for (auto item : info->vma.get_list())
            {
                trace::warning(trace::hex(item.start), "-", trace::hex(item.end), " ", trace::hex(item.flags));
            }
        }
    }
    return irq::request_result::no_handled;
}

irq::request_result _ctx_interrupt_ page_fault_func(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    using flags = arch::paging::page_fault_flags;
    if (inter->error_code & flags::user)
    {
    }
    if (inter->error_code & flags::write)
    {
        // if (inter->kernel_space)
        // {
        //     trace::panic("page ", trace::hex(extra_data), " is not writeable. code ", inter->error_code);
        // }
        // is COW page?
        if (inter->error_code & flags::present)
        {
            // trace::warning("page ", trace::hex(extra_data), " is not writeable");
            return page_fault_cow(inter, extra_data, user_data);
        }
    }
    if (!(inter->error_code & flags::present))
    {
        return page_fault_present(inter, extra_data, user_data);
    }
    if (inter->error_code & flags::instruction_fetch)
    {
        if (inter->kernel_space)
        {
            trace::panic("kernel space execute fail at page ", trace::hex(extra_data));
        }
    }
    return irq::request_result::no_handled;
}

void init() {}

void listen_page_fault() { irq::register_request_func(arch::exception::vector::page_fault, page_fault_func, 0); }

template <typename _T> _T *new_page_table()
{
    static_assert(sizeof(_T) == memory::page_size, "type _T must be a page table");
    return memory::New<_T, freelibcxx::Allocator *, memory::page_size>(memory::KernelBuddyAllocatorV);
}

template <typename _T> void delete_page_table(_T *addr)
{
    static_assert(sizeof(_T) == memory::page_size, "type _T must be a page table");
    memory::Delete<_T, freelibcxx::Allocator *>(memory::KernelBuddyAllocatorV, addr);
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
    vm_t vm(p);
    auto it = list.upper_find(vm);
    if (it != list.end())
    {
        if (it->start == p)
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
    if (unlikely(start < range_bottom))
        return nullptr;
    if (unlikely(end > range_top))
        return nullptr;

    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);

    if (list.empty())
    {
        return &list.insert(vm_t(start, end, flags, func, user_data));
    }

    // check if exist
    vm_t vm(start, end, flags, func, user_data);
    auto it = list.upper_find(vm);
    if (it != list.end() && it->start == start)
    {
        return nullptr;
    }
    it = list.upper_find(vm);
    if (it != list.end() && it->end == end)
    {
        return nullptr;
    }

    auto p = &list.insert(vm);
    return p;
}

vm_t *vm_allocator::get_vm_area(u64 p)
{
    uctx::RawReadLockUninterruptibleContext ctx(list_lock);

    vm_t vm(p, p, 0, nullptr, 0);
    auto it = list.upper_find(vm);
    if (it == list.end())
    {
        return nullptr;
    }
    if (it->start <= p && it->end > p)
    {
        return &it;
    }
    // trace::info("not find ", trace::hex(p), " ", trace::hex(it->start), "-", trace::hex(it->end));
    return nullptr;
}
void vm_allocator::clone(info_t *info, vm_allocator &to, flag_t flag)
{
    uctx::RawReadLockUninterruptibleContext ctx(list_lock);
    to.range_bottom = this->range_bottom;
    to.range_top = this->range_top;
    for (auto &item : list)
    {
        item.flags |= flag;
        auto new_item = item;
        if (item.flags & flags::file)
        {
            map_t *mt = (map_t *)item.user_data;
            new_item.user_data = (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, mt->file, mt->file_offset,
                                                         mt->file_length, mt->mmap_length, info);
        }
        else
        {
            new_item.user_data = (u64)info;
        }
        to.list.insert(new_item);
    }
}

mmu_paging::mmu_paging() { base_paging_addr = new_page_table<arch::paging::base_paging_t>(); }

mmu_paging::~mmu_paging() { delete_page_table((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::load_paging() { arch::paging::load((arch::paging::base_paging_t *)base_paging_addr); }

void fill_attr(u64 flags, u64 &attr, u64 &page_size)
{
    attr = 0;
    if (flags & flags::writeable)
    {
        attr |= arch::paging::flags::writable;
    }
    if (flags & flags::user_mode)
    {
        attr |= arch::paging::flags::user_mode;
    }
    if (flags & flags::disable_cache)
    {
        attr |= arch::paging::flags::cache_disable;
    }
    page_size = arch::paging::frame_size::size_4kb;
    if (flags & flags::big_page)
    {
        page_size = arch::paging::frame_size::size_2mb;
        attr |= arch::paging::flags::big_page;
    }
    if (flags & flags::huge_page)
    {
        page_size = arch::paging::frame_size::size_1gb;
        attr |= arch::paging::flags::big_page;
    }
}

void mmu_paging::map_area(const vm_t *vm, bool override)
{
    if (unlikely(vm == nullptr))
        return;

    u64 page_size = 0, attr = 0;
    fill_attr(vm->flags, attr, page_size);
    for (char *start = (char *)vm->start; start < (char *)vm->end; start += page_size)
    {
        phy_addr_t p = memory::va2pa(memory::KernelBuddyAllocatorV->allocate(page_size, page_size));

        arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, start, p, page_size, 1, attr, override);
    }
}

void mmu_paging::map_area_phy(const vm_t *vm, phy_addr_t start, bool override)
{
    if (unlikely(vm == nullptr))
        return;
    u64 page_size = 0, attr = 0;
    fill_attr(vm->flags, attr, page_size);

    for (byte *vstart = (byte *)vm->start; vstart < (byte *)vm->end; vstart += page_size, start += page_size)
    {
        arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, vstart, start, page_size, 1, attr, override);
    }
}

int mmu_paging::remapping(const vm_t *vm, void *vir, phy_addr_t phy)
{
    u64 page_size = 0, attr = 0;
    fill_attr(vm->flags, attr, page_size);
    arch::paging::map((arch::paging::base_paging_t *)base_paging_addr, vir, phy, page_size, 1, attr, true);
    return 0;
}
bool mmu_paging::has_shared(void *vir)
{
    return arch::paging::has_shared((arch::paging::base_paging_t *)base_paging_addr, vir);
}
bool mmu_paging::clear_shared(void *vir, bool writeable)
{
    return arch::paging::clear_shared((arch::paging::base_paging_t *)base_paging_addr, vir, writeable);
}

void mmu_paging::unmap_area(const vm_t *vm, bool free)
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
            phy_addr_t phy;
            if (arch::paging::get_map_address((arch::paging::base_paging_t *)base_paging_addr, vir, &phy))
            {
                arch::paging::unmap((arch::paging::base_paging_t *)base_paging_addr, (void *)vir,
                                    arch::paging::frame_size::size_4kb, 1);
                if (free)
                {
                    memory::free_page(memory::pa2va(phy));
                }
            }
        }
    }
    else
    {
        u64 page_count = (vm->end - vm->start) / page_size;
        for (u64 i = 0; i < page_count; i++)
        {
            auto vir = (void *)(vm->start + i * page_size);
            phy_addr_t phy;
            if (arch::paging::get_map_address((arch::paging::base_paging_t *)base_paging_addr, vir, &phy))
            {
                if (free)
                {
                    memory::free_page(memory::pa2va(phy));
                }
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

void *mmu_paging::get_base_page() { return base_paging_addr; }

bool mmu_paging::virtual_address_mapped(void *addr)
{
    phy_addr_t phy;
    return arch::paging::get_map_address((arch::paging::base_paging_t *)base_paging_addr, addr, &phy);
}

void mmu_paging::sync_kernel()
{
    uctx::UninterruptibleContext icu;
    arch::paging::sync_kernel_page_table(
        (arch::paging::base_paging_t *)base_paging_addr,
        (arch::paging::base_paging_t *)memory::kernel_vm_info->mmu_paging.base_paging_addr);
}

void mmu_paging::clone(mmu_paging &to, int deep)
{
    uctx::UninterruptibleContext icu;
    arch::paging::share_page_table((arch::paging::base_paging_t *)to.base_paging_addr,
                                   (arch::paging::base_paging_t *)base_paging_addr, 0, memory::user_mmap_top_address,
                                   true, deep);
}

info_t::info_t()
    : vma(memory::user_mmap_top_address, memory::user_code_bottom_address)
    , head_vm(nullptr)
    , shared_info(nullptr)
    , current_head_ptr(0)
{
}

info_t::~info_t()
{
    uctx::RawSpinLockUninterruptibleContext ctx(shared_info_lock);
    if (shared_info != nullptr)
    {
        shared_info->shared_pid.remove(task::current()->process->pid);
        if (shared_info->shared_pid.size() == 0)
        {
            memory::Delete(memory::KernelCommonAllocatorV, shared_info);
        }
        uctx::RawWriteLockUninterruptibleContext ctx(vma.get_lock());
        auto &list = vma.get_list();
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            mmu_paging.unmap_area(&it, false);
        }
    }
    else
    {
        uctx::RawWriteLockUninterruptibleContext ctx(vma.get_lock());
        auto &list = vma.get_list();
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            mmu_paging.unmap_area(&it, false);
        }
    }
}

bool head_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item);

bool info_t::init_brk(u64 start)
{
    head_vm = vma.add_map(start, start + memory::user_head_size,
                          memory::vm::flags::readable | memory::vm::flags::writeable | memory::vm::flags::expand |
                              memory::vm::flags::user_mode,
                          memory::vm::head_expand_vm, 0);
    if (head_vm == nullptr)
    {
        return false;
    }
    current_head_ptr = head_vm->start;
    return true;
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
        mmu_paging.unmap_area(&vm, false);
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
            memset(ptr, 0, memory::page_size);
            vm_t vm = *head_vm;
            vm.start = current_map;
            vm.end = current_map + memory::page_size;
            mmu_paging.map_area_phy(&vm, memory::va2pa(ptr), true);
        }
    }
    else
    {
        /// remove pages
        vm_t vm = *head_vm;
        vm.start = ptr;
        vm.end = current_head_ptr;
        mmu_paging.unmap_area(&vm, false);
    }
    current_head_ptr = ptr;
    return true;
}

u64 info_t::get_brk() { return current_head_ptr; }

bool head_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item)
{
    auto info = (info_t *)task::current_process()->mm_info;
    if (info->get_brk() > page_addr)
    {
        vm_t vm = *item;
        vm.start = (page_addr) & ~(memory::page_size - 1);
        vm.end = vm.start + memory::page_size;
        byte *ptr = (byte *)memory::malloc_page();
        memset(ptr, 0, memory::page_size);
        info->mmu_paging.map_area_phy(&vm, memory::va2pa(ptr), true);
        return true;
    }
    return false;
}

bool fill_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item)
{
    auto info = (info_t *)task::current_process()->mm_info;

    if (item->flags & flags::expand)
    {
        vm_t vm = *item;
        item->alloc_times++;
        int load_pages = 1;
        if (item->alloc_times > 2)
        {
            // perfect more pages
            load_pages = 10;
            if (item->alloc_times > 5)
            {
                load_pages = 30;
            }
            if (item->alloc_times > 12)
            {
                load_pages = 120;
            }
        }
        vm.start = (page_addr) & ~(memory::page_size - 1);
        if (load_pages > 1)
        {
            uint64_t page_index = (vm.start - item->start) / memory::page_size;
            uint64_t pages = (item->end - item->start) / memory::page_size;
            if (page_index + load_pages > pages)
            {
                load_pages = pages - page_index;
            }
        }
        if (vm.start == 0x11028c000)
        {
            trace::info("a");
        }
        kassert(!((item->flags & flags::big_page) || (item->flags & flags::huge_page)),
                "expand vm not allowed big paging");
        int perfect_num = 0;
        for (int index = 0; index < load_pages; index++)
        {
            if (!info->mmu_paging.virtual_address_mapped(reinterpret_cast<void *>(vm.start)))
            {
                vm.end = vm.start + memory::page_size;
                byte *ptr = (byte *)memory::malloc_page();
                if (ptr != nullptr)
                {
                    memset(ptr, 0, memory::page_size);
                    info->mmu_paging.map_area_phy(&vm, memory::va2pa(ptr), true);
                    perfect_num++;
                }
                else
                {
                    trace::panic("OOM");
                }
            }
            vm.start += memory::page_size;
        }
        if (load_pages > 1 && perfect_num > 0)
        {
            item->alloc_times -= perfect_num / 2;
        }

        return true;
    }
    return false;
}

bool fill_file_vm(vm_allocator &vma, u64 page_addr, vm_t *item)
{
    map_t *mt = (map_t *)item->user_data;
    u64 page_start = (page_addr) & ~(memory::page_size - 1);

    u64 length_read = page_start - item->start;

    byte *buffer = (byte *)memory::malloc_page();

    u64 length_can_read = length_read > mt->file_length ? 0 : mt->file_length - length_read;

    auto ksize = mt->file->pread(mt->file_offset + length_read, buffer,
                                 length_can_read > memory::page_size ? memory::page_size : length_can_read, 0);
    if (ksize == -1)
    {
        ksize = 0;
    }
    // auto *thread = cpu::current().get_task();

    // trace::info("fill ", trace::hex(page_start), " from file ", trace::hex(mt->file_offset + length_read), "+",
    //             trace::hex(ksize), " can read ", trace::hex(length_can_read), " mmap length ",
    //             trace::hex(mt->mmap_length), " process ", thread->process->pid, " cpu ", cpu::current().id(),
    //             " page addr ", trace::hex(page_addr));

    kassert(!((item->flags & flags::big_page) || (item->flags & flags::huge_page)), "file vm not allowed big paging");
    memset(buffer + ksize, 0, memory::page_size - ksize);
    vm_t vm = *item;
    vm.start = page_start;
    vm.end = vm.start + memory::page_size;
    mt->vm_info->mmu_paging.map_area_phy(&vm, memory::va2pa(buffer), true);

    if (mt->vm_info->mmu_paging.get_base_page() != memory::kernel_vm_info->mmu_paging.get_base_page())
    {
        if (page_addr >= memory::kernel_mmap_bottom_address && page_addr <= memory::kernel_mmap_top_address)
        {
            trace::info("map double ", trace::hex(page_addr));
            memory::kernel_vm_info->mmu_paging.map_area_phy(&vm, memory::va2pa(buffer), true);
        }
    }
    return true;
}

const vm_t *info_t::map_file(u64 start, fs::vfs::file *file, u64 file_offset, u64 file_length, u64 mmap_length,
                             flag_t page_ext_attr)
{
    if (is_kernel_space_pointer(start))
    {
        return nullptr;
    }

    u64 alen = (mmap_length + memory::page_size - 1) & ~(memory::page_size - 1);
    auto cflags = flags::lock | flags::user_mode | flags::expand | page_ext_attr;
    vm_page_fault_func func = fill_expand_vm;
    u64 user_data = (u64)this;

    if (file)
    {
        cflags |= flags::file;
        func = fill_file_vm;
        user_data = (u64)memory::KernelCommonAllocatorV->New<map_t>(file, file_offset, file_length, mmap_length, this);
    }

    const vm_t *vm = nullptr;
    if (start == 0)
    {
        vm = vma.allocate_map(alen, cflags, func, user_data);
    }
    else
    {
        vm = vma.add_map(start, start + alen, cflags, func, user_data);
    }
    if (!vm)
    {
        if (file)
        {
            memory::KernelCommonAllocatorV->Delete(reinterpret_cast<map_t *>(user_data));
        }
    }
    return vm;
}

void info_t::sync_map_file(u64 addr) {}

bool info_t::umap_file(u64 addr, u64 size)
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
    mmu_paging.unmap_area(vm, false);
    arch::paging::reload();
    return true;
}

void info_t::share_to(pid_t from_id, pid_t to_id, info_t *info)
{
    uctx::RawSpinLockUninterruptibleContext ctx(shared_info_lock);
    if (shared_info == nullptr)
    {
        shared_info = memory::New<shared_info_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        shared_info->shared_pid.insert(from_id);
    }
    {
        uctx::RawSpinLockUninterruptibleContext ctx2(shared_info->spin_lock);
        shared_info->shared_pid.insert(to_id);
        vma.clone(info, info->vma, flags::cow);
        mmu_paging.clone(info->mmu_paging, 100);
    }
    info->shared_info = shared_info;
}

bool info_t::copy_at(u64 vir)
{
    auto vm = vma.get_vm_area(vir);
    if (vm != nullptr)
    {
        // TODO: big page COW
        void *vir_ptr = reinterpret_cast<void *>(vir & ~(memory::page_size - 1));
        if (vm->flags & vm::flags::cow)
        {
            uctx::RawSpinLockUninterruptibleContext ctx(shared_info_lock);
            if (shared_info == nullptr)
            {
                return false;
            }
            // bool has_shared = false;
            uctx::RawSpinLockUninterruptibleContext ctx2(shared_info->spin_lock);
            // for (auto &pid : shared_info->shared_pid)
            // {
            //     auto process = ::task::find_pid(pid.key);
            //     if (process)
            //     {
            //         auto info = (info_t *)process->mm_info;
            //         if (info != this && info->mmu_paging.has_shared(vir_ptr))
            //         {
            //             has_shared = true;
            //             break;
            //         }
            //     }
            //     else
            //     {
            //         trace::panic("no pid found when cow");
            //     }
            // }
            // if (!has_shared)
            // {
            //     // reuse page
            //     for (auto &pid : shared_info->shared_pid)
            //     {
            //         auto process = ::task::find_pid(pid.key);
            //         if (process)
            //         {
            //             auto info = (info_t *)process->mm_info;
            //             auto vm = info->vma.get_vm_area(vir);
            //             info->mmu_paging.clear_shared(vir_ptr, vm->flags & flags::writeable);
            //         }
            //         else
            //         {
            //             trace::panic("no pid found when cow");
            //         }
            //     }
            //     return true;
            // }
            byte *ptr = (byte *)memory::malloc_page();
            if (mmu_paging.remapping(vm, vir_ptr, memory::va2pa(ptr)) != 0)
            {
                return true;
            }
            return true;
        }
    }
    return false;
}

} // namespace memory::vm
