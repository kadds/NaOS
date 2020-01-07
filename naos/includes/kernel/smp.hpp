#pragma once
#include "common.hpp"
namespace SMP
{
void init();
void flush_all_tlb();
void reschedule_cpu(u32 cpuid);

} // namespace SMP
