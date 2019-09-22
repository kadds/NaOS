#include "kernel/mm/vm.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/irq.hpp"
#include "kernel/task.hpp"
namespace memory::vm
{
void _ctx_interrupt_ page_fault_func(const arch::idt::regs_t *regs, u64 extra_data, u64 user_data)
{
    if (regs->rsp <= arch::task::user_stack_start_address && regs->rsp >= arch::task::user_stack_maximum_end_address)
    {
        auto *thread = task::current();
        if (thread == nullptr)
        {
            // TODO: Stack overflow
        }
        else
        {
            auto &mmu_paging = thread->process->mm_info->mmu_paging;
            auto vm = mmu_paging.get_vm_area((void *)extra_data);
            if (vm != nullptr)
            {
                // add stack
                mmu_paging.map_area(vm);
                mmu_paging.load_paging();
            }
            else
            {
                // TODO: Page fault
            }
        }
    }
}

void init() { irq::insert_request_func(arch::exception::vector::page_fault, page_fault_func, 0); }

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

mmu_paging::mmu_paging()
    : node_cache(memory::KernelBuddyAllocatorV)
    , node_cache_allocator(&node_cache)
    , list(&node_cache_allocator)
{
    base_paging_addr = new_page_table<arch::paging::pml4t>();
    arch::paging::copy_page_table((arch::paging::pml4t *)base_paging_addr, arch::paging::base_kernel_page_addr);
}

mmu_paging::~mmu_paging() { delete_page_table((arch::paging::pml4t *)base_paging_addr); }

void mmu_paging::load_paging() { arch::paging::load((arch::paging::pml4t *)base_paging_addr); }

const vm_t *mmu_paging::add_vm_area(void *start, void *end, u64 flags)
{
    kassert((char *)start < (char *)end, "parameter start must < parameter end");
    kassert((u64)start == ((u64)start & ~(memory::page_size - 1)), "parameter start must aligned");
    kassert((u64)end == ((u64)end & ~(memory::page_size - 1)), "parameter end must < parameter end");

    if (list.begin() == list.end())
    {
        return &list.push_back(vm_t(start, end, flags, nullptr))->element;
    }
    if ((char *)start < (char *)list.front().start)
    {
        return &list.insert(list.begin(), vm_t(start, end, flags, nullptr))->element;
    }
    for (auto it = list.begin(); it != list.end(); it = list.next(it))
    {
        if ((char *)start >= (char *)it->element.end)
        {
            auto next = list.next(it);
            if (next != list.end())
            {
                if ((char *)next->element.start < (char *)end)
                {
                    trace::panic("Can't insert a map area ", start, "-", end, " which inserted before.");
                }
            }
            return &list.insert(it, vm_t(start, end, flags, nullptr))->element;
        }
    }
    return nullptr;
}

u64 mmu_paging::remove_vm_area(void *start, void *end)
{
    kassert((char *)start < (char *)end, "parameter start must < parameter end");
    for (auto it = list.begin(); it != list.end(); it = list.next(it))
    {
        if ((char *)end > (char *)it->element.start && (char *)end <= (char *)it->element.end &&
            (char *)start >= (char *)it->element.start)
        {
            if (start == it->element.start)
            {
                // remove left area
                it->element.end = end;
            }
            else if (end == it->element.end)
            {
                it->element.start = start;
            }
            else
            {
                it->element.start = start;
                it->element.end = end;
            }

            break;
        }
    }
    return 0;
}

const vm_t *mmu_paging::get_vm_area(void *p)
{
    for (auto it = list.begin(); it != list.end(); it = list.next(it))
    {
        if ((char *)it->element.start < (char *)p)
        {
            if ((char *)it->element.end > (char *)p)
            {
                return &it->element;
            }
        }
    }
    return nullptr;
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

        arch::paging::map((arch::paging::pml4t *)base_paging_addr, start, phy_addr, arch::paging::frame_size::size_4kb,
                          1, attr);
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

    for (char *start = (char *)vm->start; start < (char *)vm->end;
         start += arch::paging::frame_size::size_4kb, phy_addr += arch::paging::frame_size::size_4kb)
    {
        arch::paging::map((arch::paging::pml4t *)base_paging_addr, start, phy_addr, arch::paging::frame_size::size_4kb,
                          1, attr);
    }
}

void mmu_paging::map_page(void *p) {}

void mmu_paging::unmap_area(const vm_t *vm)
{
    if (unlikely(vm == nullptr))
        return;
}
} // namespace memory::vm
