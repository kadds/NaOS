#include "kernel/arch/paging.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/mm.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
namespace arch::paging
{

static_assert(sizeof(pml4t) == 0x1000 && sizeof(pdpt) == 0x1000 && sizeof(pdt) == 0x1000 && sizeof(pt) == 0x1000,
              "sizeof paging struct is not 4KB.");

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

void *base_entry::get_addr() const
{
    return memory::kernel_phyaddr_to_virtaddr((void *)((u64)data & 0xFFFFFFFFFF000UL));
}

void *base_entry::get_phy_addr() const { return (void *)((u64)data & 0xFFFFFFFFFF000UL); }

void base_entry::set_addr(void *ptr)
{
    data = (data & ~0xFFFFFFFFFF000UL) | (((u64)memory::kernel_virtaddr_to_phyaddr(ptr)) & 0xFFFFFFFFFF000UL);
}

void base_entry::set_phy_addr(void *ptr) { data = (data & ~0xFFFFFFFFFF000UL) | (((u64)ptr) & 0xFFFFFFFFFF000UL); }

u64 get_bits(u64 addr, u8 start_bit, u8 bit_count) { return (addr >> start_bit) & ((1 << (bit_count + 1)) - 1); }

Unpaged_Text_Section void temp_init(bool is_bsp)
{
    // map 0x000000-0xffffffff->0x000000-0xffffffff,0-4GB->0-4GB
    // map 0xffff800000000000-0xffff8000ffffffff->0x000000-0xffffffff
    const u64 addr = 0x94000;
    void *temp_pml4_addr = (void *)addr;
    if (is_bsp)
    {
        u64 *page_temp_addr = (u64 *)addr;
        for (int i = 0; i < 512; i++)
            page_temp_addr[i] = 0;
        page_temp_addr[0] = addr + 0x1003;
        page_temp_addr[256] = page_temp_addr[0];
        page_temp_addr = (u64 *)(addr + 0x1000);
        for (int i = 0; i < 512; i++)
            page_temp_addr[i] = 0;
        page_temp_addr[0] = addr + 0x2003;
        page_temp_addr[1] = addr + 0x3003;
        page_temp_addr[2] = addr + 0x4003;
        page_temp_addr[3] = addr + 0x5003;

        page_temp_addr = (u64 *)(addr + 0x2000);

        u64 v = 0x83;
        // map 4GB
        for (int i = 0; i < 512 * 4; i++)
        {
            *page_temp_addr++ = v;
            v += 0x200000; // 2MB
        }
    }

    __asm__ __volatile__("movq %0, %%cr3	\n\t" : : "r"(temp_pml4_addr) : "memory");
}

void init()
{
    auto base_kernel_page_addr = (base_paging_t *)memory::kernel_vm_info->mmu_paging.get_page_addr();
    if (!cpu::current().is_bsp())
    {
        load(base_kernel_page_addr);
        return;
    }
    u64 max_maped_memory = memory::get_max_maped_memory();
    if (max_maped_memory > memory::max_memory_support)
        trace::panic("Not support such a large memory. Current maximum memory map detected ", max_maped_memory,
                     ". Maximum memory supported ", memory::max_memory_support, ".");
    // map all phy address
    if (cpu_info::has_future(cpu_info::future::huge_page_1gb))
    {
        trace::debug("Paging at 1GB granularity");
        map(base_kernel_page_addr, (void *)0xffff800000000000, (void *)0x0, frame_size::size_1gb,
            (max_maped_memory + frame_size::size_1gb - 1) / frame_size::size_1gb, flags::writable);
    }
    else
    {
        trace::debug("Paging at 2MB granularity");
        map(base_kernel_page_addr, (void *)0xffff800000000000, (void *)0x0, frame_size::size_2mb,
            (max_maped_memory + frame_size::size_2mb - 1) / frame_size::size_2mb, flags::writable);
    }
    trace::debug("Map ", (void *)0, "-", (void *)max_maped_memory, "->", (void *)0xffff800000000000, "-",
                 (void *)(0xffff800000000000 + max_maped_memory));

    trace::debug("Reload page table");
    load(base_kernel_page_addr);
    memory::kernel_vm_info->mmu_paging.load_paging();
}

void check_pml4e(pml4t *base_addr, int pml4_index)
{
    auto &pml4e = (*base_addr)[pml4_index];
    if (!pml4e.is_present())
    {
        pml4e.set_present();
        pml4e.set_user_mode();
        pml4e.set_writable();

        pml4e.set_addr(new_page_table<pdpt>());
        pml4e.set_common_data(0);
    }
}

void check_pdpe(pml4t *base_addr, int pml4_index, int pdpt_index, bool create_next_table)
{
    check_pml4e(base_addr, pml4_index);
    auto &pml4e = (*base_addr)[pml4_index];
    auto &pdpte = pml4e.next()[pdpt_index];
    if (!pdpte.is_present())
    {
        pdpte.set_present();
        pdpte.set_user_mode();
        pdpte.set_writable();

        if (create_next_table)
            pdpte.set_addr(new_page_table<pdt>());
        else
            pdpte.set_addr(0);

        pdpte.set_common_data(0);
        pml4e.set_common_data(pml4e.get_common_data() + 1);
    }
}

void check_pde(pml4t *base_addr, int pml4_index, int pdpt_index, int pt_index, bool create_next_table)
{
    check_pdpe(base_addr, pml4_index, pdpt_index, true);
    auto &pdpt = (*base_addr)[pml4_index].next()[pdpt_index];
    auto &pdt = pdpt.next()[pt_index];
    if (!pdt.is_present())
    {
        pdt.set_present();
        pdt.set_user_mode();
        pdt.set_writable();

        if (create_next_table)
            pdt.set_addr(new_page_table<pt>());
        else
            pdt.set_addr(0);

        pdt.set_common_data(0);
        pdpt.set_common_data(pdpt.get_common_data() + 1);
    }
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

bool map(base_paging_t *base_paging_addr, void *virt_start_addr, void *phy_start_addr, u64 frame_size, u64 frame_count,
         u32 page_ext_flags)
{
    uctx::UninterruptibleContext icu;
    // virtual address doesn't align of 4kb
    if (((u64)virt_start_addr & (frame_size::size_4kb - 1)) != 0)
        error_map();

    auto &top_page = *(pml4t *)base_paging_addr;
    u64 v = (u64)virt_start_addr;
    u64 pml4e_index = get_bits(v, 39, 8);
    u64 pdpe_index = get_bits(v, 30, 8);
    u64 pde_index = get_bits(v, 21, 8);
    u64 pte_index = get_bits(v, 12, 8);

    u8 *phy_addr = (u8 *)phy_start_addr;

    switch (frame_size)
    {
        case frame_size::size_1gb: {
            check_pml4e(&top_page, pml4e_index);
            for (u32 i = 0; i < frame_count; i++, pdpe_index++, phy_addr += (u32)frame_size)
            {
                if (pdpe_index >= 512)
                {
                    if (++pml4e_index >= 512)
                        error_map();
                    check_pml4e(&top_page, pml4e_index);
                    pdpe_index = 0;
                }
                auto &pml4e = top_page[pml4e_index];
                if (pml4e.next()[pdpe_index].is_present())
                    error_map();
                pml4e.next()[pdpe_index] = pdpt_entry(phy_addr, flags::big_page | flags::present | page_ext_flags);
                pml4e.set_common_data(pml4e.get_common_data() + 1);
            }
        }
        break;
        case frame_size::size_2mb: {
            check_pdpe(&top_page, pml4e_index, pdpe_index, true);
            for (u32 i = 0; i < frame_count; i++, pde_index++, phy_addr += (u32)frame_size)
            {
                if (pde_index >= 512)
                {
                    if (++pdpe_index >= 512)
                    {
                        if (++pml4e_index >= 512)
                            error_map();
                        check_pml4e(&top_page, pml4e_index);
                        pdpe_index = 0;
                    }
                    check_pdpe(&top_page, pml4e_index, pdpe_index, true);
                    pde_index = 0;
                }
                auto &pml4e = top_page[pml4e_index];
                auto &pdpe = pml4e.next()[pdpe_index];
                if (pdpe.next()[pde_index].is_present())
                    error_map();
                pdpe.next()[pde_index] = pd_entry(phy_addr, flags::big_page | flags::present | page_ext_flags);
                pdpe.set_common_data(pdpe.get_common_data() + 1);
            }
        }
        break;
        case frame_size::size_4kb: {
            check_pde(&top_page, pml4e_index, pdpe_index, pde_index, true);
            for (u32 i = 0; i < frame_count; i++, pte_index++, phy_addr += (u32)frame_size)
            {
                if (pte_index >= 512)
                {
                    if (++pde_index >= 512)
                    {
                        if (++pdpe_index >= 512)
                        {
                            if (++pml4e_index >= 512)
                                error_map();
                            check_pml4e(&top_page, pml4e_index);
                            pdpe_index = 0;
                        }
                        check_pdpe(&top_page, pml4e_index, pdpe_index, true);
                        pde_index = 0;
                    }
                    check_pde(&top_page, pml4e_index, pdpe_index, pde_index, true);
                    pte_index = 0;
                }
                auto &pml4e = top_page[pml4e_index];
                auto &pdpe = pml4e.next()[pdpe_index];
                auto &pde = pdpe.next()[pde_index];
                if (pde.next()[pte_index].is_present())
                    error_map();
                pde.next()[pte_index] = pt_entry(phy_addr, flags::present | page_ext_flags);
                pde.set_common_data(pde.get_common_data() + 1);
            }
        }
        break;
        default:
            return false;
    }
    return true;
}

void clean_null_page_pml4e(pml4t &base_page, u64 pml4e_index)
{
    auto &e = base_page[pml4e_index];
    if (e.get_common_data() == 0 && e.is_present())
    {
        delete_page_table(&e.next());
        e.clear_present();
    }
}

void clean_null_page_pdpe(pml4t &base_page, u64 pml4e_index, u64 pdpe_index)
{
    auto &e = base_page[pml4e_index].next()[pdpe_index];
    if (e.get_common_data() == 0 && e.is_present())
    {
        delete_page_table(&e.next());
        e.clear_present();
    }
}

void clean_null_page_pde(pml4t &base_page, u64 pml4e_index, u64 pdpe_index, u64 pde_index)
{
    auto &e = base_page[pml4e_index].next()[pdpe_index].next()[pde_index];
    if (e.get_common_data() == 0 && e.is_present())
    {
        delete_page_table(&e.next());
        e.clear_present();
    }
}

bool unmap(base_paging_t *base_paging_addr, void *virt_start_addr, u64 frame_size, u64 frame_count)
{
    uctx::UninterruptibleContext icu;
    // virtual address doesn't align of 4kb
    if (((u64)virt_start_addr & (frame_size::size_4kb - 1)) != 0)
        error_unmap();

    auto &top_page = *(pml4t *)base_paging_addr;
    u64 v = (u64)virt_start_addr;
    u64 pml4e_index = get_bits(v, 39, 8);
    u64 pdpe_index = get_bits(v, 30, 8);
    u64 pde_index = get_bits(v, 21, 8);
    u64 pte_index = get_bits(v, 12, 8);

    switch (frame_size)
    {
        case frame_size::size_1gb: {
            if (!top_page[pml4e_index].is_present())
                error_unmap();

            for (u32 i = 0; i < frame_count; i++, pdpe_index++)
            {
                if (pdpe_index >= 512)
                {
                    clean_null_page_pml4e(top_page, pml4e_index);
                    if (++pml4e_index >= 512)
                        error_unmap();
                    if (!top_page[pml4e_index].is_present())
                        error_unmap();
                    pdpe_index = 0;
                }
                top_page[pml4e_index].next()[pdpe_index].clear_present();
                top_page[pml4e_index].set_common_data(top_page[pml4e_index].get_common_data() - 1);
            }

            clean_null_page_pml4e(top_page, pml4e_index);
        }
        break;
        case frame_size::size_2mb: {
            if (!top_page[pml4e_index].is_present())
                error_unmap();
            if (!top_page[pml4e_index].next()[pdpe_index].is_present())
                error_unmap();

            for (u32 i = 0; i < frame_count; i++, pde_index++)
            {
                if (pde_index >= 512)
                {
                    clean_null_page_pdpe(top_page, pml4e_index, pdpe_index);
                    if (++pdpe_index >= 512)
                    {
                        clean_null_page_pml4e(top_page, pml4e_index);
                        if (++pml4e_index >= 512)
                            error_unmap();
                        if (!top_page[pml4e_index].is_present())
                            error_unmap();
                        pdpe_index = 0;
                    }
                    if (!top_page[pml4e_index].next()[pdpe_index].is_present())
                        error_unmap();
                    pde_index = 0;
                }
                auto &e = top_page[pml4e_index].next()[pdpe_index];
                e.next()[pde_index].clear_present();
                e.set_common_data(e.get_common_data() - 1);
            }
            // clean null page tables
            clean_null_page_pdpe(top_page, pml4e_index, pdpe_index);
            clean_null_page_pml4e(top_page, pml4e_index);
        }
        break;
        case frame_size::size_4kb: {
            if (!top_page[pml4e_index].is_present())
                error_unmap();
            if (!top_page[pml4e_index].next()[pdpe_index].is_present())
                error_unmap();
            if (!top_page[pml4e_index].next()[pdpe_index].next()[pde_index].is_present())
                error_unmap();

            for (u32 i = 0; i < frame_count; i++, pte_index++)
            {
                if (pte_index >= 512)
                {
                    clean_null_page_pde(top_page, pml4e_index, pdpe_index, pde_index);
                    if (++pde_index >= 512)
                    {
                        clean_null_page_pdpe(top_page, pml4e_index, pdpe_index);
                        if (++pdpe_index >= 512)
                        {
                            clean_null_page_pml4e(top_page, pml4e_index);
                            if (++pml4e_index >= 512)
                                error_unmap();
                            if (!top_page[pml4e_index].is_present())
                                error_unmap();
                            pdpe_index = 0;
                        }
                        if (!top_page[pml4e_index].next()[pdpe_index].is_present())
                            error_unmap();
                        pde_index = 0;
                    }
                    if (!top_page[pml4e_index].next()[pdpe_index].next()[pde_index].is_present())
                        error_unmap();
                    pte_index = 0;
                }
                auto &e = top_page[pml4e_index].next()[pdpe_index].next()[pde_index];
                e.next()[pte_index].clear_present();
                e.set_common_data(e.get_common_data() - 1);
            }
            clean_null_page_pde(top_page, pml4e_index, pdpe_index, pde_index);
            clean_null_page_pdpe(top_page, pml4e_index, pdpe_index);
            clean_null_page_pml4e(top_page, pml4e_index);
        }
        break;
        default:
            return false;
    }
    return true;
}

void load(base_paging_t *base_paging_addr)
{
    __asm__ __volatile__("movq %0, %%cr3	\n\t"
                         :
                         : "r"(memory::kernel_virtaddr_to_phyaddr(base_paging_addr))
                         : "memory");
}

void reload()
{
    u64 c = 0;
    __asm__ __volatile__("movq %%cr3, %0	\n\t"
                         "movq %0, %%cr3	\n\t"
                         : "+g"(c)
                         :
                         : "memory");
}

base_paging_t *current()
{
    u64 v;
    __asm__ __volatile__("movq %%cr3, %0	\n\t" : "=r"(v) : : "memory");
    return (base_paging_t *)memory::kernel_phyaddr_to_virtaddr(v);
}

template <typename PageTable> bool copy(PageTable *dst, PageTable *src, int idx)
{
    auto &source = src->entries[idx];
    if (likely(source.is_present()))
    {
        auto &to = dst->entries[idx];
        if (to.is_present())
        {
            if (likely(to.get_addr() != source.get_addr()))
            {
                delete_page_table<PageTable>((PageTable *)to.get_addr());
            }
            else
            {
                return false;
            }
        }
        to = source;
        to.set_addr(new_page_table<PageTable>());
        return true;
    }
    return false;
}

/// TODO: fill page table
void copy_page_table(base_paging_t *to, base_paging_t *source, u64 start, u64 end, bool override)
{
    pml4t *dst = (pml4t *)to;
    pml4t *src = (pml4t *)source;

    u64 pml4e_index = get_bits(start, 39, 8);
    u64 pdpe_index = get_bits(start, 30, 8);
    u64 pde_index = get_bits(start, 21, 8);
    u64 pte_index = get_bits(start, 12, 8);
    u64 page_count = (end - start) / memory::page_size;
    if (override)
    {
        for (u64 i = 0; i < page_count; i++)
        {
            if (copy(dst, src, pml4e_index))
            {
                auto &pdpt_src = src->entries[pml4e_index].next();
                auto &pdpt_dst = dst->entries[pml4e_index].next();
                if (copy(&pdpt_dst, &pdpt_src, pdpe_index))
                {
                    auto &pdt_src = pdpt_src.entries[pdpe_index].next();
                    auto &pdt_dst = pdpt_dst.entries[pdpe_index].next();
                    if (copy(&pdt_dst, &pdt_src, pde_index))
                    {
                        auto &pet_src = pdt_src.entries[pml4e_index].next();
                        auto &pet_dst = pdt_dst.entries[pml4e_index].next();
                        copy(&pet_dst, &pet_src, pte_index);
                    }
                }
            }
        }
        pte_index++;
        if (pte_index >= 512)
        {
            pte_index = 0;
            pde_index++;
            if (pde_index >= 512)
            {
                pde_index = 0;
                pdpe_index++;
                if (pdpe_index >= 512)
                {
                    pdpe_index = 0;
                    pml4e_index++;
                    if (pml4e_index >= 512)
                        error_map();
                }
            }
        }
    }
}

void sync_kernel_page_table(base_paging_t *to, base_paging_t *kernel)
{
    // 0xFFFF800000000000-0xFFFFFFFFFFFFFFFF
    pml4t *dst = (pml4t *)to;
    pml4t *src = (pml4t *)kernel;
    for (int i = 256; i < 512; i++)
    {
        auto &pml4e_source = src->entries[i];
        if (pml4e_source.is_present() && unlikely(pml4e_source.get_addr() != dst->entries[i].get_addr()))
        {
            /// TODO: free page table
        }
        dst->entries[i] = pml4e_source;
    }
}

bool get_map_address(base_paging_t *base_paging_addr, void *virt_addr, void **phy_addr)
{
    u64 start = (u64)virt_addr;
    u64 pml4e_index = get_bits(start, 39, 8);
    u64 pdpe_index = get_bits(start, 30, 8);
    u64 pde_index = get_bits(start, 21, 8);
    u64 pte_index = get_bits(start, 12, 8);
    auto &pml4e = ((pml4t *)base_paging_addr)->entries[pml4e_index];
    if (likely(pml4e.is_present()))
    {
        auto &pdpe = pml4e.next()[pdpe_index];
        if (likely(pdpe.is_present()))
        {
            if (pdpe.is_big_page())
            {
                *phy_addr = (void *)pdpe.get_phy_addr();
                return true;
            }
            auto &pde = pdpe.next()[pde_index];
            if (likely(pde.is_present()))
            {
                if (pde.is_big_page())
                {
                    *phy_addr = (void *)pde.get_phy_addr();
                    return true;
                }
                auto &pe = pde.next()[pte_index];
                if (likely(pe.is_present()))
                {
                    *phy_addr = (void *)pe.get_phy_addr();
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace arch::paging
