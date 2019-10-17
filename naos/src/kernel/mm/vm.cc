#include "kernel/mm/vm.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace memory::vm
{
irq::request_result _ctx_interrupt_ page_fault_func(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data)
{
    if (!arch::cpu::current().is_in_user_context((void *)regs->rsp))
        return irq::request_result::no_handled;
    auto *thread = arch::cpu::current().get_task();
    if (thread == nullptr)
    {
        trace::debug("no thread page");
    }
    else
    {
        auto &mmu_paging = thread->process->mm_info->mmu_paging;
        auto vm = mmu_paging.get_vma().get_vm_area(extra_data);
        if (vm != nullptr)
        {
            // add stack
            mmu_paging.expand_area(vm, (void *)extra_data);
            mmu_paging.load_paging();
            return irq::request_result::ok;
        }
        else
        {
            // TODO: Page fault
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

const vm_t *vm_allocator::allocate_map(u64 size, u64 flags)
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
                return &list.insert(it, vm_t(vm->end, vm->end + size, flags, 0));
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
        return &list.push_back(vm_t(range_top - size, range_top, flags, 0));
    }
    return nullptr;
}

vm_t vm_allocator::deallocate_map(void *ptr)
{
    vm_t t(0, 0, 0, 0);
    uctx::SpinLockUnInterruptableContext ctx(list_lock);

    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if ((char *)it->start < (char *)ptr)
        {
            if ((char *)it->end > (char *)ptr)
            {
                t = *it;
                list.remove(it);
                return t;
            }
        }
    }
    return t;
}

const vm_t *vm_allocator::add_map(u64 start, u64 end, u64 flags)
{

    kassert((char *)start < (char *)end, "parameter start must < parameter end");
    kassert((u64)start == ((u64)start & ~(memory::page_size - 1)), "parameter start must aligned");
    kassert((u64)end == ((u64)end & ~(memory::page_size - 1)), "parameter end must < parameter end");
    uctx::SpinLockUnInterruptableContext ctx(list_lock);

    if (list.begin() == list.end())
    {
        return &list.push_back(vm_t(start, end, flags, nullptr));
    }
    if ((char *)start < (char *)list.front().start)
    {
        return &list.insert(list.begin(), vm_t(start, end, flags, nullptr));
    }
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if ((char *)start >= (char *)it->end)
        {
            auto next = it;
            ++next;
            if (next != list.end())
            {
                if ((char *)next->start < (char *)end)
                {
                    trace::panic("Can't insert a map area ", start, "-", end, " which inserted before.");
                }
            }
            return &list.insert(it, vm_t(start, end, flags, nullptr));
        }
    }
    return nullptr;
}

const vm_t *vm_allocator::get_vm_area(u64 p)
{
    uctx::SpinLockUnInterruptableContext ctx(list_lock);
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if ((char *)it->start < (char *)p)
        {
            if ((char *)it->end > (char *)p)
            {
                return &it;
            }
        }
    }
    return nullptr;
}

mmu_paging::mmu_paging()
    : vma(memory::user_mmap_top_address, memory::user_code_bottom_address)
{
    base_paging_addr = new_page_table<arch::paging::base_paging_t>();
    arch::paging::copy_page_table((arch::paging::base_paging_t *)base_paging_addr, arch::paging::get_kernel_paging());
}

mmu_paging::~mmu_paging() { delete_page_table((arch::paging::base_paging_t *)base_paging_addr); }

void mmu_paging::clean_user_mmap()
{
    uctx::SpinLockUnInterruptableContext ctx(vma.get_lock());

    for (auto it = vma.get_list().begin(); it != vma.get_list().end();)
    {
        if (it->flags & flags::user_mode)
        {
            it = vma.get_list().remove(it);
        }
        else
        {
            ++it;
        }
    }
}

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

    uctx::SpinLockUnInterruptableContext ctx(vma.get_lock());

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

    uctx::SpinLockUnInterruptableContext ctx(vma.get_lock());

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

    uctx::SpinLockUnInterruptableContext ctx(vma.get_lock());

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
    uctx::SpinLockUnInterruptableContext ctx(vma.get_lock());

    arch::paging::unmap((arch::paging::base_paging_t *)base_paging_addr, (void *)vm->start,
                        arch::paging::frame_size::size_4kb, (vm->end - vm->start) / arch::paging::frame_size::size_4kb);
}

} // namespace memory::vm
