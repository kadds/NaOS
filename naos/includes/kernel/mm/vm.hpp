#pragma once
#include "common.hpp"

namespace memory
{
class mmu_paging
{
  private:
    void *base_paging_addr;

  public:
    void set_base_paging_addr(void *paging_addr);
    void reload_paging();
};
} // namespace memory
