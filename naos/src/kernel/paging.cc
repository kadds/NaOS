#include "kernel/paging.hpp"
#include "kernel/memory.hpp"

namespace paging
{

ExportC void _load_page(void *);
ExportC void _reload_segment(u64 cs, u64 ss);
pml4e *page_addr;

void init()
{
    page_addr = (pml4e *)memory::alloc(sizeof(pml4e) * 512, 0x1000);

    // PML4T -> PDPT
    auto pdpt_addr = (pdpte *)memory::alloc(sizeof(pdpte) * 512, 0x1000);
    page_addr[0] = pml4e(pdpt_addr, pml4e::present | pml4e::read_write | pml4e::user_supervisor);
    for (int i = 1; i < 512; i++)
        page_addr[i] = pml4e();
    page_addr[256] = page_addr[0];

    // PDPT -> PDT
    auto pde_addr = (pde *)memory::alloc(sizeof(pde) * 512, 0x1000);
    pdpt_addr[0] = pdpte(pde_addr, pdpte::present | pdpte::read_write);
    for (int i = 1; i < 512; i++)
        pdpt_addr[i] = pdpte();

    // map 0x000000-0x9fffff->0x000000-0x9fffff,0-10MB->0-10MB
    for (int i = 0; i < 5; i++)
    {
        pde_addr[i] = pde((void *)(pde::big_page_size * i), pde::present | pde::read_write | pde::big_page);
    }
    // map 0xa00000-0x19fffff->0xe0000000-0xe0ffffff,11-27MB->3.50GB-3.51GB
    for (int i = 0; i < 8; i++)
    {
        pde_addr[i + 5] =
            pde((void *)(pde::big_page_size * i + 0xe0000000), pde::present | pde::read_write | pde::big_page);
    }
    for (int i = 0; i < 499; i++)
    {
        pde_addr[i + 13] = pde();
    }
    _load_page(page_addr);
}
} // namespace paging
