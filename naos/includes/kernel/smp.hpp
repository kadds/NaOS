#pragma once
#include "cpu.hpp"
#include "kernel/common.hpp"
namespace SMP
{

void init();

/// tlb shutdown
void flush_all_tlb();

void reschedule_cpu(u32 cpuid);

/// call per cpu function
void call_cpu(u32 cpuid, cpu::call_cpu_func_t, u64 user_data);

} // namespace SMP
