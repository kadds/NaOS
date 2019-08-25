#include "kernel/mm/vm.hpp"
#include "kernel/arch/paging.hpp"
namespace memory
{
void mmu_paging::set_base_paging_addr(void *base_paging_addr) { this->base_paging_addr = base_paging_addr; }
void mmu_paging::reload_paging() { paging::load((paging::pml4t *)base_paging_addr); }
} // namespace memory
