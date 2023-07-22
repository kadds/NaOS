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
            arch::paging::page_table_t::reload();
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

        auto vm = info->vma().get_vm_area(extra_data);
        if (vm != nullptr)
        {
            // if (vm->flags & flags::cow) {
            //     return irq::request_result::no_handled;
            // }
            u64 alignment_page = memory::align_down(extra_data, page_size);
            if (!info->expand(vm->method, alignment_page, extra_data, vm))
            {
                return irq::request_result::no_handled;
            }
            arch::paging::page_table_t::reload();
            return irq::request_result::ok;
        }
        else
        {
            trace::info("vm area not found ", trace::hex(extra_data), " at process ", thread->process->pid, " by ",
                        trace::hex(inter->at));
            for (auto item : info->vma().get_list())
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
    kassert(!(inter->error_code & flags::reserved_write), inter->error_code);
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

const vm_t *vm_allocator::allocate_map(u64 size, u64 flags, page_fault_method method, u64 user_data)
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
                return &list.insert(vm_t(low_bound, low_bound + size, flags, method, user_data));
            }
            low_bound = vm->end;
        }
    }
    if (range_top < low_bound + size)
    {
        return nullptr;
    }
    return &list.insert(vm_t(low_bound, low_bound + size, flags, method, user_data));
}

void vm_allocator::deallocate_map(const vm_t *vm)
{
    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    list.remove(*vm);
}

bool vm_allocator::deallocate_map(u64 p)
{
    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    vm_t vm(p, p, 0);
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

vm_allocator::~vm_allocator() {}

const vm_t *vm_allocator::add_map(u64 start, u64 end, u64 flags, page_fault_method func, u64 user_data)
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

    vm_t vm(p, p, 0);
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

u64 to_paging_flags(u64 flags)
{
    u64 paging_flags = 0;
    if (flags & flags::writeable)
    {
        paging_flags |= arch::paging::flags::writable;
    }
    if (flags & flags::user_mode)
    {
        paging_flags |= arch::paging::flags::user_mode;
    }
    if (flags & flags::disable_cache)
    {
        paging_flags |= arch::paging::flags::cache_disable;
    }
    return paging_flags;
}

info_t::info_t()
    : vma_(memory::user_mmap_top_address, memory::user_code_bottom_address)
    , heap_vm_(nullptr)
    , heap_top_(0)
{
}

info_t::info_t(arch::paging::page_table_t paging)
    : vma_(memory::user_mmap_top_address, memory::user_code_bottom_address)
    , paging_(std::move(paging))
    , heap_vm_(nullptr)
    , heap_top_(0)
{
}

info_t::~info_t()
{
    uctx::RawWriteLockUninterruptibleContext ctx(vma_.get_lock());
    uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
    auto &list = vma_.get_list();
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        // trace::info("umap ", trace::hex(it->start), "-", trace::hex(it->end));
        paging_.unmap(reinterpret_cast<void *>(it->start), (it->end - it->start) / page_size);
    }
}

bool head_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item);

bool info_t::init_brk(u64 start)
{
    heap_vm_ = vma_.add_map(start, start + memory::user_head_size,
                            memory::vm::flags::readable | memory::vm::flags::writeable | memory::vm::flags::expand |
                                memory::vm::flags::user_mode,
                            page_fault_method::heap_break, 0);
    if (heap_vm_ == nullptr)
    {
        return false;
    }
    heap_top_ = heap_vm_->start;
    return true;
}

bool info_t::set_brk(u64 ptr)
{
    ptr = (ptr + memory::page_size - 1) & ~(memory::page_size - 1);

    if (ptr > heap_vm_->end || ptr < heap_vm_->start)
    {
        return false;
    }

    if (ptr > heap_top_)
    {
        /// add page
        /// \see head_expand_vm
    }
    else
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        /// remove pages
        paging_.unmap(reinterpret_cast<void *>(ptr), (heap_top_ - ptr) / page_size);
    }
    heap_top_ = ptr;
    return true;
}

bool info_t::set_brk_now(u64 ptr)
{
    ptr = (ptr + memory::page_size - 1) & ~(memory::page_size - 1);

    if (ptr > heap_vm_->end || ptr < heap_vm_->start)
    {
        return false;
    }

    if (ptr > heap_top_)
    {
        /// add page
        auto current_map = heap_top_;
        while (ptr < current_map)
        {
            uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
            u64 page_flags = to_paging_flags(heap_vm_->flags);
            paging_.map(reinterpret_cast<void *>(current_map), 1, page_flags, 0);
        }
    }
    else
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        /// remove pages
        paging_.unmap(reinterpret_cast<void *>(ptr), (heap_top_ - ptr) / page_size);
    }
    heap_top_ = ptr;
    return true;
}

u64 info_t::get_brk() { return heap_top_; }

bool info_t::expand(page_fault_method method, u64 alignment_page, u64 access_address, vm_t *item)
{
    switch (method)
    {
        case page_fault_method::none:
            return false;
        case page_fault_method::common:
            return expand_vm(alignment_page, access_address, item);
        case page_fault_method::common_with_bss:
            return expand_bss(alignment_page, access_address, item);
        case page_fault_method::heap_break:
            return expand_brk(alignment_page, access_address, item);
        case page_fault_method::file:
            return expand_file(alignment_page, access_address, item);
        default:
            return false;
    }
}

bool info_t::expand_brk(u64 alignment_page, u64 access_address, vm_t *item)
{
    if (access_address < heap_top_)
    {
        u64 page_flags = to_paging_flags(item->flags);
        {
            uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
            paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
        }
        return true;
    }
    return false;
}

bool info_t::expand_vm(u64 alignment_page, u64 access_address, vm_t *item)
{
    if (!(item->flags & flags::expand))
    {
        return false;
    }
    u64 page_flags = to_paging_flags(item->flags);
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
    }
    // auto phy = paging_.get_map(reinterpret_cast<void *>(alignment_page)).value();
    // memset(pa2va(phy), 0, memory::page_size);
    return true;
}

bool info_t::expand_bss(u64 alignment_page, u64 access_address, vm_t *item)
{
    if (!(item->flags & flags::expand))
    {
        return false;
    }
    u64 page_flags = to_paging_flags(item->flags);

    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
        auto phy = paging_.get_map(reinterpret_cast<void *>(alignment_page)).value();
        memset(pa2va(phy), 0, memory::page_size);
    }

    return true;
}

bool info_t::expand_file(u64 alignment_page, u64 access_address, vm_t *item)
{
    map_t *mt = (map_t *)item->user_data;
    u64 length_read = alignment_page - item->start;
    u64 page_flags = to_paging_flags(item->flags);
    byte *buffer;
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
        auto phy = paging_.get_map(reinterpret_cast<void *>(alignment_page)).value();
        buffer = (byte *)pa2va(phy);
    }

    u64 length_can_read = length_read > mt->file_length ? 0 : mt->file_length - length_read;
    auto ksize = mt->file->pread(mt->file_offset + length_read, buffer,
                                 length_can_read > memory::page_size ? memory::page_size : length_can_read, 0);
    if (ksize == -1)
    {
        ksize = 0;
    }
    memset(buffer + ksize, 0, memory::page_size - ksize);
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
    page_fault_method method = page_fault_method::common;
    u64 user_data = (u64)this;

    if (file)
    {
        cflags |= flags::file;
        method = page_fault_method::file;
        user_data = (u64)memory::KernelCommonAllocatorV->New<map_t>(file, file_offset, file_length, mmap_length, this);
    }

    const vm_t *vm = nullptr;
    if (start == 0)
    {
        vm = vma().allocate_map(alen, cflags, method, user_data);
    }
    else
    {
        vm = vma().add_map(start, start + alen, cflags, method, user_data);
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
    auto vm = vma_.get_vm_area(addr);
    if (!vm)
        return false;
    if (vm->flags & flags::file)
    {
        map_t *mt = (map_t *)vm->user_data;
        memory::Delete<>(memory::KernelCommonAllocatorV, mt);
    }
    vma_.deallocate_map(vm);

    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.unmap(reinterpret_cast<void *>(vm->start), (vm->end - vm->start) / page_size);
    }

    arch::paging::page_table_t::reload();
    return true;
}

void info_t::share_to(process_id from_id, process_id to_id, info_t *info)
{
    vma_.clone(info, info->vma_, flags::cow);
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.clone_readonly_to((void *)0, memory::user_mmap_top_address / memory::page_size, &info->paging_);
    }
}

bool info_t::copy_at(u64 virt_addr)
{
    auto vm = vma_.get_vm_area(virt_addr);
    if (vm != nullptr)
    {
        if (!(vm->flags & vm::flags::writeable))
        {
            return false;
        }
        // TODO: big page COW
        u64 alignment_page = align_down(virt_addr, memory::page_size);
        if (vm->flags & vm::flags::cow)
        {
            uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
            // trace::info("cow at ", trace::hex(alignment_page), " at ", task::current_process()->pid);
            u64 page_flags = to_paging_flags(vm->flags);
            paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags,
                        arch::paging::action_flags::override | arch::paging::action_flags::cow);
            return true;
        }
    }
    return false;
}

} // namespace memory::vm
