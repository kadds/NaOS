#include "kernel/arch/paging.hpp"
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/kernel.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/mm/zone.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
#include <termios.h>
#include <type_traits>
namespace arch::paging
{

static_assert(sizeof(pml4t) == 0x1000 && sizeof(pdpt) == 0x1000 && sizeof(pdt) == 0x1000 && sizeof(pt) == 0x1000,
              "sizeof paging struct is not 4KB.");

template <typename T>
concept page_table = requires(T t)
{
    t[0];
    t.is_page_table();
};

template <typename PageTable>
requires page_table<PageTable> memory::page *page_table2page(PageTable &base)
{
    return memory::global_zones->get_page(&base);
}

template <typename T>
requires page_table<T> T *new_page_table()
{
    static_assert(sizeof(T) == 0x1000, "type _T must be a page table");
    auto p = memory::New<T, freelibcxx::Allocator *, 0x1000>(memory::KernelBuddyAllocatorV);
    memory::global_zones->get_page(p)->set_page_table_counter(0);
    return p;
}

template <typename T>
requires page_table<T>
void delete_page_table(T *addr)
{
    static_assert(sizeof(T) == 0x1000, "type _T must be a page table");
    memory::Delete<T, freelibcxx::Allocator *>(memory::KernelBuddyAllocatorV, addr);
}

void *base_entry::get_addr() const { return memory::pa2va(phy_addr_t::from((u64)data & 0xF'FFFF'FFFF'F000UL)); }

phy_addr_t base_entry::get_phy_addr() const { return phy_addr_t::from((void *)((u64)data & 0xF'FFFF'FFFF'F000UL)); }

void base_entry::set_addr(void *ptr)
{
    data = (data & ~0xF'FFFF'FFFF'F000UL) | (((u64)memory::va2pa(ptr)()) & 0xF'FFFF'FFFF'F000UL);
}

void base_entry::set_phy_addr(void *ptr)
{
    data = (data & ~0xF'FFFF'FFFF'F000UL) | (((u64)ptr) & 0xF'FFFF'FFFF'F000UL);
}

inline int get_bits(u64 addr, u8 start_bit, u8 bit_count) { return (addr >> start_bit) & ((1 << (bit_count + 1)) - 1); }

lock::spinlock_t share_spin;

template <typename PageTable, typename PageEntry>
requires page_table<PageTable>
bool share_page_entry(PageTable &base, PageEntry &entry, u64 flags, u64 actions)
{
    auto readonly_flags = flags & ~flags::writable;
    if (readonly_flags == flags && !(actions & action_flags::copy_all))
    {
        // nothing to share
        return false;
    }
    uctx::RawSpinLockUninterruptibleContext icu(share_spin);
    auto &next = entry.next();
    if (memory::global_zones->get_page_reference(&next) == 1)
    {
        entry.set_flags(flags);
        return false;
    }

    using NextPageTable = std::remove_reference_t<decltype(entry.next())>;

    int old_counter = page_table2page(next)->page_table_counter();
    kassert(counter(next) == old_counter, "counter not match ", counter(next), "!=", old_counter);

    auto p = new_page_table<NextPageTable>();

    // copy next level page entry
    int new_counter = 0;
    for (int i = 0; i < 512 && new_counter < old_counter; i++)
    {
        if (next[i].is_present())
        {
            memory::page *page = memory::global_zones->get_page(next[i].get_addr());
            page->add_ref_count();

            next[i].set_flags(readonly_flags);
            (*p)[i] = next[i];
            new_counter++;
        }
    }
    kassert(new_counter == old_counter, new_counter, "!=", old_counter);

    delete_page_table(&next);

    page_table2page(*p)->set_page_table_counter(new_counter);
    entry.set_next(p);
    entry.set_flags(flags);

    return true;
}

template <typename PageTable>
requires page_table<PageTable>
int counter(PageTable &base)
{
    int n = 0;
    for (int i = 0; i < 512; i++)
    {
        if (base[i].is_present())
        {
            n++;
        }
    }
    return n;
}

bool move_forward_index(int level, int &pml4e_index, int &pdpe_index, int &pde_index, int &pte_index) { return false; }

Unpaged_Text_Section u64 get_bits_unpaged(u64 addr, u8 start_bit, u8 bit_count)
{
    return (addr >> start_bit) & ((1 << (bit_count + 1)) - 1);
}

Unpaged_Text_Section void set_zero(void *p)
{
    u64 *page_entries = (u64 *)p;
    for (int i = 0; i < 512; i++)
    {
        page_entries[i] = 0;
    }
}

Unpaged_Data_Section(1) static u64 page_alloc_position;

Unpaged_Text_Section void fill_stack_page_table(u64 base_virtual_addr, u64 phy_addr)
{
    u64 *page_entries = (u64 *)base_tmp_page_entries;
    u64 pc = memory::kernel_stack_page_count;
    for (u64 i = 0; i < pc; i++)
    {
        u64 pml4e_index = get_bits_unpaged(base_virtual_addr, 39, 8);
        u64 pdpe_index = get_bits_unpaged(base_virtual_addr, 30, 8);
        u64 pde_index = get_bits_unpaged(base_virtual_addr, 21, 8);
        u64 pte_index = get_bits_unpaged(base_virtual_addr, 12, 8);

        u64 *page_pdp_entries = (u64 *)(page_entries[pml4e_index] & 0xF'FFFF'FFFF'F000UL);

        if (page_pdp_entries == 0)
        {
            page_alloc_position += 0x1000;
            page_pdp_entries = (u64 *)page_alloc_position;
            set_zero(page_pdp_entries);
            page_entries[pml4e_index] = page_alloc_position | 0x3;
        }

        u64 *page_pd_entries = (u64 *)(page_pdp_entries[pdpe_index] & 0xF'FFFF'FFFF'F000UL);
        if (page_pd_entries == 0)
        {
            page_alloc_position += 0x1000;
            page_pd_entries = (u64 *)page_alloc_position;
            set_zero(page_pd_entries);
            page_pdp_entries[pdpe_index] = page_alloc_position | 0x3;
        }

        u64 *page_pt_entries = (u64 *)(page_pd_entries[pde_index] & 0xF'FFFF'FFFF'F000UL);
        if (page_pt_entries == 0)
        {
            page_alloc_position += 0x1000;
            page_pt_entries = (u64 *)page_alloc_position;
            set_zero(page_pt_entries);
            page_pd_entries[pde_index] = page_alloc_position | 0x3;
        }

        page_pt_entries[pte_index] = phy_addr | 0x83;

        base_virtual_addr += memory::page_size;
        phy_addr += memory::page_size;
    }
}

Unpaged_Text_Section void temp_init(bool is_bsp)
{
    // map 0x0 -> 4GB
    // map 0xFFFF800000000000 -> 4GB
    // also map cpu_stack_bottom_address[0] -> 0x8XFFF-0x90000

    void *temp_pml4_addr = (void *)base_tmp_page_entries;
    if (is_bsp)
    {
        page_alloc_position = reinterpret_cast<u64>(&base_tmp_page_entries);
        u64 *page_entries = (u64 *)page_alloc_position;
        set_zero(page_entries);
        u64 target_phy = 0x83;

        page_alloc_position += 0x1000;
        page_entries[0] = page_alloc_position + 3;
        page_entries[256] = page_alloc_position + 3;
        u64 *page_pdp_entries = (u64 *)page_alloc_position;
        set_zero(page_pdp_entries);

        for (int i = 0; i < 4; i++)
        {
            page_alloc_position += 0x1000;
            page_pdp_entries[i] = page_alloc_position + 3;

            u64 *page_pd_entries = (u64 *)page_alloc_position;
            for (int k = 0; k < 512; k++)
            {
                page_pd_entries[k] = target_phy;
                target_phy += 0x200000;
            }
        }

        __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(temp_pml4_addr) : "memory");

        u64 base = memory::kernel_cpu_stack_bottom_address + memory::page_size;
        fill_stack_page_table(base, 0x90000 - memory::page_size * memory::kernel_stack_page_count);

        __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(temp_pml4_addr) : "memory");
    }
    else
    {
        __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(temp_pml4_addr) : "memory");
        auto phy = arch::SMP::ap_stack_phy;
        auto base = arch::SMP::ap_stack;
        fill_stack_page_table((u64)base, (u64)phy);
        __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(temp_pml4_addr) : "memory");
    }
}

void init()
{
    auto &kernel_paging = memory::kernel_vm_info->paging();
    if (!cpu::current().is_bsp())
    {
        kernel_paging.load();
        return;
    }
    kernel_paging.prepare_kernel_space();

    u64 max_maped_memory = memory::get_max_maped_memory();
    if (max_maped_memory > memory::max_memory_support)
        trace::panic("Not support such a large memory. Current maximum memory map detected ", max_maped_memory,
                     ". Maximum memory supported ", memory::max_memory_support, ".");
    // map all phy address
    auto virt_start = reinterpret_cast<void *>(memory::minimum_kernel_addr);

    if (cpu_info::has_feature(cpu_info::feature::huge_page_1gb))
    {
        max_maped_memory = memory::align_up(max_maped_memory, frame_size::size_1gb);
        auto pages = max_maped_memory / memory::page_size;
        kernel_paging.huge_page_map_to(virt_start, pages, phy_addr_t::from(0), arch::paging::flags::writable, 0);
        trace::debug("Paging at 1GB granularity");
    }
    else
    {
        max_maped_memory = memory::align_up(max_maped_memory, frame_size::size_2mb);
        auto pages = max_maped_memory / memory::page_size;
        kernel_paging.big_page_map_to(virt_start, pages, phy_addr_t::from(0), arch::paging::flags::writable, 0);
        trace::debug("Paging at 2MB granularity");
    }
    trace::debug("Map address ", trace::hex(0), "-", trace::hex(max_maped_memory), "->", virt_start, "-",
                 trace::hex(memory::minimum_kernel_addr + max_maped_memory));
}

NoReturn void error_map()
{
    trace::panic("Unsupported operation at virtual address map. A virtual address has been mapped, out of range or "
                 "not aligned.");
}

NoReturn void error_unmap()
{
    trace::panic("Unsupported operation at virtual address unmap. A virtual address hasn't been mapped, out of range "
                 "or not aligned.");
}

pml4t *current()
{
    u64 v;
    __asm__ __volatile__("movq %%cr3, %0	\n\t" : "=r"(v) : :);
    return memory::pa2va<pml4t *>(phy_addr_t::from(v));
}

page_table_t::page_table_t()
    : self_(true)
{
    base_ = new_page_table<pml4t>();
}
page_table_t &page_table_t::operator=(page_table_t &&rhs)
{
    if (&rhs == this)
    {
        return *this;
    }
    if (base_ != nullptr && self_)
    {
        delete_page_table<pml4t>(base_);
    }

    base_ = rhs.base_;
    self_ = rhs.self_;

    rhs.base_ = nullptr;

    return *this;
}

page_table_t::~page_table_t()
{
    if (base_ != nullptr && self_)
    {
        delete_page_table<pml4t>(base_);
    }
}

void page_table_t::load() { __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(memory::va2pa(base_)()) : "memory"); }

void page_table_t::reload()
{
    u64 c = 0;
    __asm__ __volatile__("movq %%cr3, %0	\n\t"
                         "movq %0, %%cr3	\n\t"
                         : "+g"(c)
                         :
                         : "memory");
}

void page_table_t::map(void *virt_start, size_t pages, u64 flags, u64 actions)
{
    // virtual address doesn't align of 4kb
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    if (p & (frame_size::size_4kb - 1))
    {
        error_map();
    }
    flags |= flags::present;

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    ensure(pml4e_index, pdpe_index, pde_index, flags, actions);

    for (size_t i = 0; i < pages; i++, pte_index++)
    {
        if (normalize_index(pml4e_index, pdpe_index, pde_index, pte_index))
        {
            ensure(pml4e_index, pdpe_index, pde_index, flags, actions);
        }

        auto &pe = (*base_)[pml4e_index][pdpe_index][pde_index][pte_index];

        auto old_addr = pe.get_addr();
        void *target_addr = old_addr;

        if (pe.is_present())
        {
            if (!(actions & action_flags::override))
            {
                error_map();
            }
            auto page = memory::global_zones->get_page(old_addr);
            if (page->get_ref_count() >= 1)
            {
                // if (page->get_ref_count() != 1)
                // {
                target_addr = memory::KernelBuddyAllocatorV->allocate(memory::page_size, 0);
                memcpy(target_addr, old_addr, frame_size::size_4kb);
                memory::KernelBuddyAllocatorV->deallocate(old_addr);
                // }
            }
            else
            {
                error_map();
            }
        }
        else
        {
            if (actions & action_flags::cow)
            {
                error_map();
            }

            target_addr = memory::KernelBuddyAllocatorV->allocate(memory::page_size, 0);
            page_table2page((*base_)[pml4e_index][pdpe_index][pde_index].next())->add_page_table_counter();
        }

        pe.set_addr(target_addr);
        pe.set_flags(flags);
    }
}

void page_table_t::map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions)
{
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    if (p & (frame_size::size_4kb - 1))
    {
        error_map();
    }
    flags |= flags::present;

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    ensure(pml4e_index, pdpe_index, pde_index, flags, actions);

    for (size_t i = 0; i < pages; i++, pte_index++)
    {
        if (normalize_index(pml4e_index, pdpe_index, pde_index, pte_index))
        {
            ensure(pml4e_index, pdpe_index, pde_index, flags, actions);
        }
        auto &pe = (*base_)[pml4e_index][pdpe_index][pde_index][pte_index];

        if (!pe.is_present())
        {
            page_table2page((*base_)[pml4e_index][pdpe_index][pde_index].next())->add_page_table_counter();
        }
        pe.set_phy_addr(phy_start());
        pe.set_flags(flags);

        phy_start += frame_size::size_4kb;
    }
}

void page_table_t::big_page_map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions)
{
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    if (p & (frame_size::size_2mb - 1))
    {
        error_map();
    }
    if (pages & (big_pages - 1))
    {
        error_map();
    }

    flags |= flags::present;

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    ensure(pml4e_index, pdpe_index, flags, actions);

    for (size_t i = 0; i < pages; i += big_pages, pde_index++)
    {
        if (normalize_index(pml4e_index, pdpe_index, pde_index, pte_index))
        {
            ensure(pml4e_index, pdpe_index, flags, actions);
        }
        auto &pde = (*base_)[pml4e_index][pdpe_index][pde_index];

        if (!pde.is_present())
        {
            page_table2page((*base_)[pml4e_index][pdpe_index].next())->add_page_table_counter();
        }
        pde.set_phy_addr(phy_start());
        pde.set_flags(flags | flags::big_page);

        phy_start += frame_size::size_2mb;
    }
}

void page_table_t::huge_page_map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions)
{
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    if (p & (frame_size::size_1gb - 1))
    {
        error_map();
    }
    if (pages & (huge_pages - 1))
    {
        error_map();
    }

    flags |= flags::present;

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    ensure(pml4e_index, flags, actions);

    for (size_t i = 0; i < pages; i += huge_pages, pdpe_index++)
    {
        if (normalize_index(pml4e_index, pdpe_index, pde_index, pte_index))
        {
            ensure(pml4e_index, flags, actions);
        }

        auto &pdpe = (*base_)[pml4e_index][pdpe_index];

        if (!pdpe.is_present())
        {
            page_table2page((*base_)[pml4e_index].next())->add_page_table_counter();
        }
        pdpe.set_phy_addr(phy_start());
        pdpe.set_flags(flags | flags::huge_page);

        phy_start += frame_size::size_1gb;
    }
}

void page_table_t::prepare_kernel_space()
{
    for (int pml4e_index = 256; pml4e_index < 512; pml4e_index++)
    {
        auto &pml4e = (*base_)[pml4e_index];
        if (pml4e.is_present())
        {
            continue;
        }
        page_table2page(*base_)->add_page_table_counter();
        pml4e.set_next(new_page_table<pdpt>());
        pml4e.set_flags(flags::writable | flags::present);
    }
}

void page_table_t::map_kernel_space()
{
    page_table_t p(current());
    for (int pml4e_index = 256; pml4e_index < 512; pml4e_index++)
    {
        auto &src = (*p.base_)[pml4e_index];
        (*base_)[pml4e_index] = src;
        if (src.is_present())
        {
            page_table2page(*base_)->add_page_table_counter();
        }
    }
}

void page_table_t::unmap(void *virt_start, size_t pages)
{
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    if (p & (frame_size::size_4kb - 1))
    {
        error_unmap();
    }

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    auto *pml4e = &(*base_)[pml4e_index];
    pdpt_entry *pdpe = nullptr;
    pd_entry *pde = nullptr;
    pt_entry *pte = nullptr;
    bool need_check = true;
    int skip_level = 1;

    for (size_t i = 0; i < pages;)
    {
        if (normalize_index(pml4e_index, pdpe_index, pde_index, pte_index))
        {
            pml4e = &(*base_)[pml4e_index];
            need_check = true;
        }
        skip_level = 3;
        bool skip = false;

        while (need_check)
        {
            if (!pml4e->is_present())
            {
                skip_level = 0;
                skip = true;
                break;
            }

            pdpe = &(*pml4e)[pdpe_index];
            if (!pdpe->is_present())
            {
                skip_level = 1;
                skip = true;
                break;
            }

            pde = &(*pdpe)[pde_index];
            if (!pde->is_present())
            {
                skip_level = 2;
                skip = true;
                break;
            }
            break;
        }
        if (!skip)
        {
            // auto rest = pages - i;
            // if (pte_index == 0 && rest >= 512)
            // {
            //     if (pde_index == 0 && rest >= 512 * 512)
            //     {
            //         if (pdpe_index == 0 && rest >= 512 * 512 * 512)
            //         {
            //             // remove pml4 entry;
            //             delete_page_table(&pml4e->next());
            //             skip_level = 0;
            //             skip = true;
            //         }
            //         else
            //         {
            //             delete_page_table(&pdpe->next());
            //             skip_level = 1;
            //             skip = true;
            //         }
            //     }
            //     else
            //     {
            //         delete_page_table(&pde->next());
            //         skip_level = 2;
            //         skip = true;
            //     }
            // }
        }
        if (!skip)
        {
            pte = &(*pde)[pte_index];
            if (pte->is_present())
            {
                ensure(pml4e_index, pdpe_index, pde_index, pte->flags(),
                       action_flags::cow | action_flags::override | action_flags::copy_all);

                pml4e = &(*base_)[pml4e_index];
                pdpe = &(*pml4e)[pdpe_index];
                pde = &(*pdpe)[pde_index];
                pte = &(*pde)[pte_index];
                need_check = true;

                auto addr = to_virt_addr(index_t::from_pack(pml4e_index, pdpe_index, pde_index, pte_index));
                kassert(memory::global_zones->get_page_reference(pml4e->get_addr()) == 1, "pml4e ", pml4e_index,
                        " check status fail ", trace::hex(addr));
                kassert(memory::global_zones->get_page_reference(pdpe->get_addr()) == 1, "pml4e ", pml4e_index,
                        " pdpe ", pdpe_index, " check status fail ", trace::hex(addr));
                kassert(memory::global_zones->get_page_reference(pde->get_addr()) == 1, "pml4e ", pml4e_index, " pdpe ",
                        pdpe_index, " pde ", pde_index, " check status fail ", trace::hex(addr));

                memory::KernelBuddyAllocatorV->deallocate(pte->get_addr());
                pte->set_flags(0);

                auto pde_page = page_table2page(pde->next());
                auto pdpe_page = page_table2page(pdpe->next());
                auto pml4e_page = page_table2page(pml4e->next());

                kassert(pde_page->page_table_counter() > 0, "invalid status");
                pde_page->sub_page_table_counter();
                if (pde_page->page_table_counter() == 0)
                {
                    delete_page_table(&pde->next());
                    pde->set_flags(0);

                    kassert(pdpe_page->page_table_counter() > 0, "invalid status");
                    pdpe_page->sub_page_table_counter();
                }

                if (pdpe_page->page_table_counter() == 0)
                {
                    delete_page_table(&pdpe->next());
                    pdpe->set_flags(0);

                    kassert(pml4e_page->page_table_counter() > 0, "invalid status");
                    pml4e_page->sub_page_table_counter();
                }

                if (pml4e_page->page_table_counter() == 0)
                {
                    delete_page_table(&pml4e->next());
                    pml4e->set_flags(0);
                }
            }
        }

        switch (skip_level)
        {
            case 0:
                pml4e_index++;
                i += (512 - pdpe_index) * 512 * 512 + (512 - pde_index) * 512 + (512 - pte_index);
                pdpe_index = 0;
                pde_index = 0;
                pte_index = 0;
                need_check = true;
                break;
            case 1:
                pdpe_index++;
                i += (512 - pde_index) * 512 + (512 - pte_index);
                pde_index = 0;
                pte_index = 0;
                need_check = true;
                break;
            case 2:
                pde_index++;
                i += 512 - pte_index;
                pte_index = 0;
                need_check = true;
                break;
            case 3:
                pte_index++;
                i++;
                break;
        }
    }
}

// return need add counter
template <typename PageTable>
requires page_table<PageTable>
bool ensure_single(PageTable &base, int index, u64 flags, u64 actions)
{
    auto &entry = base[index];
    using NextPageTable = std::remove_reference_t<decltype(entry.next())>;

    if (entry.is_present())
    {
        if ((entry.flags() | flags) != entry.flags() && actions & (action_flags::cow | action_flags::override))
        {
            flags |= entry.flags();
        }
        flags &= ~(flags::accessed | flags::dirty);

        return share_page_entry(base, entry, flags, actions);
    }
    else
    {
        auto p = new_page_table<NextPageTable>();
        page_table2page(base)->add_page_table_counter();
        entry.set_next(p);
        entry.set_flags(flags);

        kassert(counter(base) == page_table2page(base)->page_table_counter(), "counter not match ", counter(base),
                "!=", page_table2page(base)->page_table_counter());
    }

    return true;
}

void page_table_t::ensure(int pml4e_index, int pdpe_index, int pde_index, u64 flags, u64 actions)
{
    ensure(pml4e_index, pdpe_index, flags, actions);
    ensure_single((*base_)[pml4e_index][pdpe_index].next(), pde_index, flags, actions);
}

void page_table_t::ensure(int pml4e_index, int pdpe_index, u64 flags, u64 actions)
{
    ensure(pml4e_index, flags, actions);
    ensure_single((*base_)[pml4e_index].next(), pdpe_index, flags, actions);
}

void page_table_t::ensure(int pml4e_index, u64 flags, u64 actions)
{
    ensure_single(*base_, pml4e_index, flags, actions);
}

bool page_table_t::has_flags(void *virt_start, u64 flags)
{
    auto p = memory::align_up(reinterpret_cast<u64>(virt_start), memory::page_size);

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();
    flags |= flags::present;

    auto &pml4e = (*base_)[pml4e_index];
    if (!(pml4e.flags() & flags))
    {
        return false;
    }

    auto &pdpe = pml4e[pdpe_index];
    if (!(pdpe.flags() & flags))
    {
        return false;
    }
    if (pdpe.is_big_page())
    {
        return true;
    }

    auto &pde = pdpe[pde_index];
    if (!(pde.flags() & flags))
    {
        return false;
    }
    if (pde.is_big_page())
    {
        return true;
    }

    auto &pte = pde[pte_index];
    return pte.flags() & flags;
}

freelibcxx::optional<phy_addr_t> page_table_t::get_map(void *virt_start)
{
    auto p = memory::align_down(reinterpret_cast<u64>(virt_start), memory::page_size);

    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    auto &pml4e = (*base_)[pml4e_index];
    if (!pml4e.is_present())
    {
        return freelibcxx::nullopt;
    }
    auto &pdpe = pml4e[pdpe_index];
    if (!pdpe.is_present())
    {
        return freelibcxx::nullopt;
    }
    if (pdpe.is_big_page())
    {
        return pdpe.get_phy_addr();
    }
    auto &pde = pdpe[pde_index];
    if (!pde.is_present())
    {
        return freelibcxx::nullopt;
    }
    if (pde.is_big_page())
    {
        return pde.get_phy_addr();
    }
    auto &pte = pde[pte_index];
    if (!pte.is_present())
    {
        return freelibcxx::nullopt;
    }
    return pte.get_phy_addr();
}

void page_table_t::clone_readonly_to(void *virt_start, size_t pages, page_table_t *to)
{
    auto p = reinterpret_cast<uintptr_t>(virt_start);
    auto index = from_virt_addr(p);
    auto [pml4e_index, pdpe_index, pde_index, pte_index] = index.unpack();

    auto to_page = page_table2page(*to->base_);
    for (size_t i = 0; i < pages; i += huge_pages)
    {
        // clone pml4e
        auto &src = (*base_)[pml4e_index];
        auto &dst = (*to->base_)[pml4e_index];
        if (!src.is_present())
        {
            continue;
        }
        auto addr = src.get_addr();

        memory::global_zones->page_add_reference(addr);
        auto flags = src.flags();
        flags &= ~flags::writable;

        src.set_flags(flags);

        dst = src;
        to_page->add_page_table_counter();

        pml4e_index++;
    }
    kassert(counter(*to->base_) == to_page->page_table_counter(), "counter not match");
}

} // namespace arch::paging
