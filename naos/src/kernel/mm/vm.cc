#include "kernel/mm/vm.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/common.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/signal.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include <limits>

KLOG_MODULE(mm);
namespace memory::vm
{

struct map_t;
bool write_back_memory_object(arch::paging::page_table_t &paging, const vm_t &vm, map_t &mapping);

irq::request_result _ctx_interrupt_ page_fault_cow(const irq::interrupt_info *inter, u64 extra_data)
{
    auto *thread = cpu::current().get_task();
    if (thread != nullptr)
    {
        auto info = (info_t *)thread->process->mm_info;

        if (is_kernel_space_pointer(extra_data))
        {
            if (!inter->kernel_space)
            {
                KLOG_WARN("process {} using kernel space pointer {}", thread->process->pid, log::hex(extra_data));
                auto &pack = thread->process->signal_pack;
                pack.send(thread->process, ::task::signal::sigstkflt, extra_data, 0, 0);
                return irq::request_result::ok;
            }
            else
            {
                KLOG_PANIC("kernel space cow");
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

irq::request_result _ctx_interrupt_ page_fault_present(const irq::interrupt_info *inter, u64 extra_data)
{
    auto *thread = cpu::current().get_task();
    if (thread != nullptr)
    {
        if (extra_data == 0)
        {
            KLOG_WARN("null pointer access pid {} tid {}", thread->process->pid, thread->tid);
        }
        auto info = (info_t *)thread->process->mm_info;

        if (is_kernel_space_pointer(extra_data))
        {
            if (!inter->kernel_space)
            {
                KLOG_WARN("process {} using kernel space pointer {}", thread->process->pid, log::hex(extra_data));
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
            KLOG_INFO("vm area not found {} at process {} by {}", log::hex(extra_data), thread->process->pid,
                      log::hex(inter->at));
            for (auto item : info->vma().get_list())
            {
                KLOG_WARN("{}-{} {}", log::hex(item.start), log::hex(item.end), log::hex(item.flags));
            }
        }
    }
    return irq::request_result::no_handled;
}

irq::request_result _ctx_interrupt_ page_fault_func(const irq::interrupt_info *inter, u64 extra_data) noexcept
{
    using flags = arch::paging::page_fault_flags;
    kassert(!(inter->error_code & flags::reserved_write), "{}", inter->error_code);
    if (inter->error_code & flags::user)
    {
    }
    if (inter->error_code & flags::write)
    {
        // if (inter->kernel_space)
        // {
        //     KLOG_PANIC("page {} is not writeable. code {}", log::hex(extra_data), inter->error_code);
        // }
        // is COW page?
        if (inter->error_code & flags::present)
        {
            // KLOG_WARN("page {} is not writeable", log::hex(extra_data));
            return page_fault_cow(inter, extra_data);
        }
    }
    if (!(inter->error_code & flags::present))
    {
        return page_fault_present(inter, extra_data);
    }
    if (inter->error_code & flags::instruction_fetch)
    {
        if (inter->kernel_space)
        {
            KLOG_PANIC("kernel space execute fail at page {}", log::hex(extra_data));
        }
    }
    if (naos::usercopy::recover_page_fault(static_cast<regs_t *>(inter->regs)))
        return irq::request_result::ok;
    return irq::request_result::no_handled;
}

void init() {}

void listen_page_fault()
{
    static irq::registration *page_fault_registration;
    page_fault_registration = memory::New<irq::registration>(memory::KernelCommonAllocatorV);
    *page_fault_registration =
        irq::register_handler(arch::exception::vector::page_fault, irq::hard_handler::bind<&page_fault_func>());
}

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
    // KLOG_INFO("not find {} {}-{}", log::hex(p), log::hex(it->start), log::hex(it->end));
    return nullptr;
}
void vm_allocator::clone(info_t *info, vm_allocator &to, flag_t flag)
{
    // The source page table is made read-only by share_to().  Both address
    // spaces therefore need the COW VMA marker so a write in either process
    // can be resolved by copy_at().
    uctx::RawWriteLockUninterruptibleContext ctx(list_lock);
    to.range_bottom = this->range_bottom;
    to.range_top = this->range_top;
    for (auto &item : list)
    {
        item.flags |= flag;
        auto new_item = item;
        if (item.flags & flags::memory_object)
        {
            map_t *mt = (map_t *)item.user_data;
            new_item.user_data = (u64)memory::New<map_t>(memory::KernelCommonAllocatorV, *mt, info);
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
        // KLOG_INFO("umap {}-{}", log::hex(it->start), log::hex(it->end));
        bool release_frames = true;
        if (it->flags & flags::memory_object)
        {
            auto *mapping = reinterpret_cast<map_t *>(it->user_data);
            if ((it->flags & flags::writeable) != 0 && !write_back_memory_object(paging_, *it, *mapping))
                KLOG_WARN("memory object write-back failed during address-space teardown");
            // Shared-page mappings point straight at the object's page cache;
            // those frames belong to the object, not to this address space.
            release_frames = !mapping->pages_shared;
            memory::Delete<>(memory::KernelCommonAllocatorV, mapping);
        }
        paging_.unmap(reinterpret_cast<void *>(it->start), (it->end - it->start) / page_size, release_frames);
    }
}

bool head_expand_vm(vm_allocator &vma, u64 page_addr, vm_t *item);

bool write_back_memory_object(arch::paging::page_table_t &paging, const vm_t &vm, map_t &mapping)
{
    // Direct-mapped device memory is written through; there is no shadow
    // buffer to flush back to the object.
    if (mapping.memory_object->physical().get() != nullptr)
        return true;
    // A mapping that faults onto the object's own page frames has no private
    // pages: every write already landed in the object.
    if (mapping.pages_shared)
        return true;
    if (!mapping.shared)
        return true;
    const u64 logical_end = mapping.data_offset + mapping.data_length;
    for (u64 page_relative = mapping.data_offset & ~(page_size - 1); page_relative < logical_end;
         page_relative += page_size)
    {
        const auto physical = paging.get_map(reinterpret_cast<void *>(vm.start + page_relative));
        if (!physical.has_value())
            continue;
        const u64 begin = mapping.data_offset > page_relative ? mapping.data_offset : page_relative;
        const u64 page_end = page_relative + page_size;
        const u64 end = logical_end < page_end ? logical_end : page_end;
        const u64 amount = end - begin;
        u64 actual = 0;
        if (mapping.memory_object->write(mapping.file_offset + begin,
                                         reinterpret_cast<const byte *>(memory::pa2va(physical.value())) +
                                             (begin - page_relative),
                                         amount, actual) != NA_STATUS_OK ||
            actual != amount)
        {
            KLOG_WARN("memory object write-back page failed at {}", log::hex(mapping.file_offset + begin));
            return false;
        }
    }
    return true;
}

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
        case page_fault_method::memory_object:
            return expand_memory_object(alignment_page, access_address, item);
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
    (void)access_address;
    if (!(item->flags & flags::expand))
    {
        return false;
    }
    u64 page_flags = to_paging_flags(item->flags);
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
        auto phy = paging_.get_map(reinterpret_cast<void *>(alignment_page));
        if (!phy.has_value())
            return false;
        memset(pa2va(phy.value()), 0, memory::page_size);
    }
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

bool info_t::expand_memory_object(u64 alignment_page, u64 access_address, vm_t *item)
{
    (void)access_address;
    auto *mapping = reinterpret_cast<map_t *>(item->user_data);
    if (mapping == nullptr || mapping->memory_object == nullptr)
        return false;

    // Direct-mapped device memory (framebuffer): fault the physical page
    // straight into the user page table with write-through/uncached
    // semantics.  No shadow buffer, no read-back, no write-back -- the
    // display memory itself is the mapping target.
    const phy_addr_t physical = mapping->memory_object->physical();
    if (physical.get() != nullptr)
    {
        const u64 relative = alignment_page - item->start;
        if (relative >= mapping->file_length)
            return false;
        const u64 page_flags =
            to_paging_flags(item->flags) | arch::paging::flags::write_through | arch::paging::flags::cache_disable;
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map_to(reinterpret_cast<void *>(alignment_page), 1, physical + relative, page_flags,
                       arch::paging::action_flags::override);
        return true;
    }

    const u64 relative = alignment_page - item->start;

    // A NA_MEMORY_MAP_SHARED mapping of a page-backed object faults the
    // object's own frame into the user page table.  Both the kernel view and
    // every other shared mapping then observe the same bytes, so no page is
    // copied in on fault and none is copied back on unmap.  map_memory_object
    // only marks a mapping pages_shared when the object covers the whole
    // range, so a missing frame here is an invariant violation: fault rather
    // than quietly handing out a private page that would not alias.
    if (mapping->pages_shared)
    {
        if (relative >= mapping->file_length)
            return false;
        const phy_addr_t frame = mapping->memory_object->page_frame(mapping->file_offset + relative);
        if (frame.get() == nullptr)
            return false;
        const u64 page_flags = to_paging_flags(item->flags);
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map_to(reinterpret_cast<void *>(alignment_page), 1, frame, page_flags,
                       arch::paging::action_flags::override);
        return true;
    }

    byte *buffer = nullptr;
    const u64 page_flags = to_paging_flags(item->flags);
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags, arch::paging::action_flags::override);
        auto physical_page = paging_.get_map(reinterpret_cast<void *>(alignment_page));
        if (!physical_page.has_value())
            return false;
        buffer = reinterpret_cast<byte *>(pa2va(physical_page.value()));
    }

    memset(buffer, 0, memory::page_size);
    if (relative >= mapping->file_length)
        return true;
    const u64 logical_end = mapping->data_offset + mapping->data_length;
    const u64 page_end = relative + memory::page_size;
    if (relative >= logical_end || page_end <= mapping->data_offset)
        return true;
    const u64 begin = relative < mapping->data_offset ? mapping->data_offset : relative;
    const u64 end = logical_end < page_end ? logical_end : page_end;
    const u64 object_offset = mapping->file_offset + begin;
    const u64 amount = end - begin;
    u64 actual = 0;
    const bool loaded =
        mapping->memory_object->read(object_offset, buffer + (begin - relative), amount, actual) == NA_STATUS_OK &&
        actual == amount;
    if (!loaded)
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.unmap(reinterpret_cast<void *>(alignment_page), 1);
    }
    return loaded;
}

const vm_t *info_t::map_memory_object(u64 start, khandle backing, naos::data_plane::memory_object *object,
                                      u64 object_offset, u64 data_offset, u64 data_length, u64 map_length,
                                      flag_t page_ext_attr)
{
    if (!backing || object == nullptr || map_length == 0 ||
        map_length > std::numeric_limits<u64>::max() - (memory::page_size - 1))
        return nullptr;
    if (data_offset > map_length || data_length > map_length - data_offset || object_offset > object->size() ||
        data_offset > object->size() - object_offset || data_length > object->size() - object_offset - data_offset)
        return nullptr;
    if (is_kernel_space_pointer(start))
        return nullptr;

    const u64 aligned_length = (map_length + memory::page_size - 1) & ~(memory::page_size - 1);
    if (start != 0 && (start > std::numeric_limits<u64>::max() - aligned_length ||
                       !is_user_space_range(reinterpret_cast<void *>(start), aligned_length)))
        return nullptr;
    const bool shared = (page_ext_attr & flags::shared) != 0;
    // SHARED mappings see each other's writes, so the object's pages must be
    // the mapping target rather than per-process private pages.  The object
    // commits to page frames once, on the first such mapping.  The mapping
    // only qualifies when the object's pages cover the entire mapped range;
    // otherwise the tail has no frame to alias.
    bool pages_shared = false;
    if (shared)
    {
        const auto publish_status = object->publish_shared_pages();
        if (publish_status != NA_STATUS_OK && publish_status != NA_STATUS_NOT_SUPPORTED)
            return nullptr;
        pages_shared = data_offset == 0 && data_length == map_length && (map_length & (memory::page_size - 1)) == 0 &&
                       object->page_backed() && map_length <= object->size() - object_offset;
    }
    const u64 mapping_flags = flags::lock | flags::user_mode | flags::expand | flags::memory_object | page_ext_attr;
    auto *mapping = memory::KernelCommonAllocatorV->New<map_t>(std::move(backing), object, object_offset, data_offset,
                                                               data_length, map_length, shared, this);
    if (mapping == nullptr)
        return nullptr;
    mapping->pages_shared = pages_shared;

    const vm_t *vm = start == 0 ? vma().allocate_map(aligned_length, mapping_flags, page_fault_method::memory_object,
                                                     reinterpret_cast<u64>(mapping))
                                : vma().add_map(start, start + aligned_length, mapping_flags,
                                                page_fault_method::memory_object, reinterpret_cast<u64>(mapping));
    if (vm == nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, mapping);
    return vm;
}

bool info_t::unmap(u64 addr, u64 size)
{
    if (size == 0 || (addr & (memory::page_size - 1)) != 0 || (size & (memory::page_size - 1)) != 0)
        return false;
    auto vm = vma_.get_vm_area(addr);
    if (!vm)
        return false;
    if ((vm->start & (memory::page_size - 1)) != 0 || vm->end <= vm->start ||
        (vm->end - vm->start) % memory::page_size != 0)
        return false;
    if (size != vm->end - vm->start)
        return false;
    const u64 vm_start = vm->start;
    const u64 vm_pages = (vm->end - vm->start) / page_size;
    const flag_t vm_flags = vm->flags;
    auto *map_data = reinterpret_cast<map_t *>(vm->user_data);

    if ((vm_flags & flags::memory_object) != 0 && (vm_flags & flags::writeable) != 0 &&
        !write_back_memory_object(paging_, *vm, *map_data))
        return false;

    // A shared-page mapping only borrows the object's frames; the object's
    // page cache frees them when the last capability closes.
    const bool release_frames = (vm_flags & flags::memory_object) == 0 || !map_data->pages_shared;
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.unmap(reinterpret_cast<void *>(vm_start), vm_pages, release_frames);
    }

    vma_.deallocate_map(vm);
    if ((vm_flags & flags::memory_object) != 0)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, map_data);
    }

    arch::paging::page_table_t::reload();
    return true;
}

void info_t::share_to(process_id from_id, process_id to_id, info_t *info)
{
    (void)from_id;
    (void)to_id;
    vma_.clone(info, info->vma_, flags::cow);
    {
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        paging_.clone_readonly_to((void *)0, memory::user_mmap_top_address / memory::page_size, &info->paging_);
        restore_fork_disallowed_mappings();
        arch::paging::page_table_t::reload();
    }
    // clone_readonly_to made every user mapping read-only and COW.  That is
    // correct for private memory, but a shared-page memory-object mapping is
    // the object's own storage in both address spaces; re-establish it
    // writable and out of COW before either process can fault on it.
    restore_shared_memory_mappings();
    info->restore_shared_memory_mappings();
    info->remove_fork_disallowed_mappings();
}

void info_t::restore_shared_memory_mappings()
{
    bool remapped = false;
    {
        uctx::RawWriteLockUninterruptibleContext vma_guard(vma_.get_lock());
        uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
        for (auto &item : vma_.get_list())
        {
            if (!(item.flags & flags::memory_object))
                continue;
            // A read-only mapping never faults on write, so COW is harmless
            // there and the PTE must stay read-only.
            if (!(item.flags & flags::writeable))
                continue;
            auto *mapping = reinterpret_cast<map_t *>(item.user_data);
            if (mapping == nullptr || !mapping->pages_shared)
                continue;
            // A mapping whose object range does not cover the whole VMA has
            // private pages past the object end; leave it entirely to COW
            // rather than leaving a non-COW VMA that cannot fault.
            if (mapping->file_length < item.end - item.start)
                continue;
            const u64 length = item.end - item.start;
            const u64 page_flags = to_paging_flags(item.flags);
            bool complete = true;
            for (u64 offset = 0; offset < length; offset += memory::page_size)
            {
                const phy_addr_t frame = mapping->memory_object->page_frame(mapping->file_offset + offset);
                if (frame.get() == nullptr)
                {
                    complete = false;
                    break;
                }
                paging_.map_to(reinterpret_cast<void *>(item.start + offset), 1, frame, page_flags,
                               arch::paging::action_flags::override);
            }
            // Only lift COW when every page was restored; a partially
            // restored VMA would fault on a write it can no longer resolve.
            if (!complete)
                continue;
            item.flags &= ~flags::cow;
            remapped = true;
        }
    }
    if (remapped)
        arch::paging::page_table_t::reload();
}

void info_t::restore_fork_disallowed_mappings() {}

void info_t::remove_fork_disallowed_mappings() {}

bool info_t::copy_at(u64 virt_addr)
{
    auto vm = vma_.get_vm_area(virt_addr);
    if (vm != nullptr)
    {
        if (!(vm->flags & vm::flags::writeable))
        {
            return false;
        }
        // A shared-page mapping is already the object's own storage in this
        // and every other address space.  Privatizing it here would break the
        // sharing contract, so the fault must stay an access violation.
        if (vm->flags & vm::flags::memory_object)
        {
            auto *mapping = reinterpret_cast<map_t *>(vm->user_data);
            if (mapping != nullptr && mapping->pages_shared)
                return false;
        }
        // TODO: big page COW
        u64 alignment_page = align_down(virt_addr, memory::page_size);
        if (vm->flags & vm::flags::cow)
        {
            uctx::RawSpinLockUninterruptibleContext icu(paging_spin_);
            u64 page_flags = to_paging_flags(vm->flags);
            // KLOG_INFO("cow at {} at {}", log::hex(alignment_page), task::current_process()->pid);
            paging_.map(reinterpret_cast<void *>(alignment_page), 1, page_flags,
                        arch::paging::action_flags::override | arch::paging::action_flags::cow);
            return true;
        }
    }
    return false;
}

} // namespace memory::vm
