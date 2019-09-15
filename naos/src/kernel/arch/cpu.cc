#include "kernel/arch/cpu.hpp"
#include "kernel/arch/klib.hpp"
namespace arch::cpu
{
cpu_data pre_cpu_data[max_cpu_support];

void init(u64 cpuid)
{
    _wrmsr(0xC0000102, (u64)((cpu_data *)&pre_cpu_data + cpuid));
    _wrmsr(0xC0000101, (u64)((cpu_data *)&pre_cpu_data + cpuid));
}
void set_pre_cpu_rsp(u64 cpuid, void *rsp) { pre_cpu_data[cpuid].kernel_rsp = rsp; }
} // namespace arch::cpu