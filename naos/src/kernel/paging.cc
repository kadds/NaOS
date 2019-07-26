#include "kernel/paging.hpp"
#include "kernel/kernel.hpp"
#include "kernel/klib.hpp"
#include "kernel/memory.hpp"
namespace paging
{

pml4t *base_page_addr;

template <typename _T> _T *new_page_table() { return memory::New<_T, 0x1000>(); }
void delete_page_table(void *addr) {}

u64 get_bits(u64 addr, u8 start_bit, u8 bit_count) { return (addr >> start_bit) & (1 << bit_count); }

Unpaged_Text_Section void temp_init()
{
    // map 0x000000-0xffffff->0x000000-0xffffff,0-16MB->0-16MB
    // map 0xffff800000000000-0xffff800000ffffff->0x000000-0xffffff
    const u64 addr = 0x80000;

    void *temp_pml4_addr = (void *)addr;
    u64 *page_temp_addr = (u64 *)addr;
    for (int i = 0; i < 512; i++)
        page_temp_addr[i] = 0;
    page_temp_addr[0] = addr + 0x1003;
    page_temp_addr[256] = page_temp_addr[0];
    page_temp_addr = (u64 *)(addr + 0x1000);
    for (int i = 0; i < 512; i++)
        page_temp_addr[i] = 0;
    page_temp_addr[0] = addr + 0x2003;

    page_temp_addr = (u64 *)(addr + 0x2000);
    for (int i = 0; i < 512; i++)
        page_temp_addr[i] = 0;
    u64 v = 0x83;
    for (int i = 0; i < 8; i++)
    {
        *page_temp_addr++ = v;
        v += 0x200000; // 2MB
    }

    _unpaged_load_page(temp_pml4_addr);
}
void init()
{
    base_page_addr = new_page_table<pml4t>();

    // map 0xffff800000000000-0xffff800000ffffff->0x000000-0xffffff
    map((void *)0xffff800000000000, (void *)0x0, frame_size_type::size_2_mb, 8, flags::writable);

    _load_page(base_page_addr);
}

void check_pdpt(pml4t *base_addr, int pml4_index)
{
    auto &pml4e = (*base_addr)[pml4_index];
    if (!pml4e.is_present())
    {
        pml4e.set_addr(new_page_table<pdpt>());
        pml4e.set_present();
    }
}
void check_pdt(pml4t *base_addr, int pml4_index, int pdpt_index)
{
    check_pdpt(base_addr, pml4_index);
    auto &pdpte = (*base_addr)[pml4_index].next()[pdpt_index];
    if (!pdpte.is_present())
    {
        pdpte.set_addr(new_page_table<pdt>());
        pdpte.set_present();
    }
}
void check_pt(pml4t *base_addr, int pml4_index, int pdpt_index, int pt_index)
{
    check_pdt(base_addr, pml4_index, pdpt_index);
    auto &pdt = (*base_addr)[pml4_index].next()[pdpt_index].next()[pt_index];
    if (!pdt.is_present())
    {
        pdt.set_addr(new_page_table<pt>());
        pdt.set_present();
    }
}

bool map(void *virt_start_addr, void *phy_start_addr, frame_size_type frame_size, u32 map_count, u32 page_ext_flags)
{
    auto &top_page = *base_page_addr;
    u64 v = (u64)virt_start_addr;
    u64 pml4e_index = get_bits(v, 39, 8);
    u64 pdpe_index = get_bits(v, 30, 8);
    u64 pde_index = get_bits(v, 21, 8);
    u64 pte_index = get_bits(v, 12, 8);
    u64 offset = get_bits(v, 0, 12);
    u8 *phy_addr = (u8 *)phy_start_addr;

    switch (frame_size)
    {
        case frame_size_type::size_1_gb:
        {
            check_pdpt(base_page_addr, pml4e_index);
            for (u32 i = 0; i < map_count; i++, pdpe_index++, phy_addr += (u32)frame_size)
            {
                if (pdpe_index > 512)
                {
                    if (++pml4e_index > 512)
                        return false;
                    check_pdpt(base_page_addr, pml4e_index);
                    pdpe_index = 0;
                }

                top_page[pml4e_index].next()[pdpe_index] =
                    pdpt_entry(phy_addr, flags::big_page | flags::present | page_ext_flags);
            }
        }
        break;
        case frame_size_type::size_2_mb:
        {
            check_pdt(base_page_addr, pml4e_index, pdpe_index);
            for (u32 i = 0; i < map_count; i++, pde_index++, phy_addr += (u32)frame_size)
            {
                if (pde_index > 512)
                {
                    if (++pdpe_index > 512)
                    {
                        if (++pml4e_index > 512)
                            return false;
                        check_pdpt(base_page_addr, pml4e_index);
                        pdpe_index = 0;
                    }
                    check_pdt(base_page_addr, pml4e_index, pdpe_index);
                    pde_index = 0;
                }

                top_page[pml4e_index].next()[pdpe_index].next()[pde_index] =
                    pd_entry(phy_addr, flags::big_page | flags::present | page_ext_flags);
            }
        }
        break;
        case frame_size_type::size_4_kb:
        {
            check_pt(base_page_addr, pml4e_index, pdpe_index, pde_index);
            for (u32 i = 0; i < map_count; i++, pte_index++, phy_addr += (u32)frame_size)
            {
                if (pte_index > 512)
                {
                    if (++pde_index > 512)
                    {
                        if (++pdpe_index > 512)
                        {
                            if (++pml4e_index > 512)
                                return false;
                            check_pdpt(base_page_addr, pml4e_index);
                            pdpe_index = 0;
                        }
                        check_pdt(base_page_addr, pml4e_index, pdpe_index);
                        pde_index = 0;
                    }
                    check_pt(base_page_addr, pml4e_index, pdpe_index, pde_index);
                    pte_index = 0;
                }

                top_page[pml4e_index].next()[pdpe_index].next()[pde_index].next()[pte_index] =
                    pt_entry(phy_addr, flags::present | page_ext_flags);
            }
        }
        break;
        default:
            return false;
    }
    return true;
}
void unmap(void *virt_addr, void *phy_addr) {}
} // namespace paging
