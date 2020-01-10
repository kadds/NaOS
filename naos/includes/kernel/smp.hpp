#pragma once
#include "common.hpp"
#include "cpu.hpp"
namespace SMP
{
void init();
void flush_all_tlb();
void reschedule_cpu(u32 cpuid);

void call_cpu(u32 cpuid, cpu::call_cpu_func, u64 user_data);

} // namespace SMP
