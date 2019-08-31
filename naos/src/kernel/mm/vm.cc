#include "kernel/mm/vm.hpp"
#include "kernel/arch/paging.hpp"
namespace memory
{
void mmu_paging::set_base_paging_addr(void *base_paging_addr) { this->base_paging_addr = base_paging_addr; }
void mmu_paging::reload_paging() { arch::paging::load((arch::paging::pml4t *)base_paging_addr); }
} // namespace memory
