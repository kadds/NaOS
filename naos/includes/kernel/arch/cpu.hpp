#pragma once
#include "common.hpp"

namespace arch::cpu
{
constexpr u32 max_cpu_support = 32;

struct cpu_data
{
    void *kernel_rsp;
}; // storeage in gs
extern cpu_data pre_cpu_data[max_cpu_support];

void init(u64 cpuid);
void set_pre_cpu_rsp(u64 cpuid, void *rsp);

} // namespace arch::cpu
