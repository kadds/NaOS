#pragma once
#include "common.hpp"
#include "mm.hpp"
#include "regs.hpp"

extern volatile char base_ap_phy_addr[];

extern volatile char base_virtual_addr[];
extern volatile char base_phy_addr[];

extern volatile char _file_start[];

extern volatile char _start_of_kernel_data[];
extern volatile char _end_kernel_data[];

extern volatile char _file_end[];

extern volatile char _bss_start[];
extern volatile char _bss_end[];

extern volatile char _bss_unpaged_start[];
extern volatile char _bss_unpaged_end[];

extern volatile char _ap_code_start[];
extern volatile char _ap_code_end[];

extern volatile char _ap_count[];
extern volatile char _ap_startup_spin_flag[];
extern volatile char _ap_stack[];

ExportC void _reload_segment(u64 cs, u64 ss);

ExportC void _cpu_id(u32 *out_eax, u32 *out_ebx, u32 *out_ecx, u32 *out_edx);

ExportC NoReturn void _kernel_thread(regs_t *regs);
ExportC void _switch_task(void *prev, void *next);

ExportC void _unpaged_reload_segment(u64 cs, u64 ss);

inline void _wrmsr(u64 address, u64 value)
{
    __asm__ __volatile__("wrmsr  \n\t"
                         :
                         : "d"((u32)(value >> 32)), "a"((u32)(value & 0xFFFFFFFF)), "c"(address)
                         : "memory");
}
inline u64 _rdmsr(u64 address)
{
    u32 v0 = 0;
    u32 v1 = 0;
    __asm__ __volatile__("rdmsr	\n\t" : "=d"(v0), "=a"(v1) : "c"(address) : "memory");
    return ((u64)v0) << 32 | v1;
}

inline u64 _rdtsc()
{
    u32 v0, v1;
    __asm__ __volatile__("mfence \n\t rdtsc	\n\t" : "=d"(v0), "=a"(v1) : : "memory");
    return ((u64)v0) << 32 | v1;
}

inline void _mfence() { __asm__ __volatile__("mfence\n\t" : : : "memory"); }

ExportC volatile char _sys_call;
ExportC volatile char _sys_ret;

ExportC NoReturn void _call_sys_ret(regs_t *regs);
ExportC void _switch_stack(u64 param1, u64 param2, u64 param3, u64 param4, void *func, void *rsp);

inline static u64 get_stack()
{
    u64 v0;
    __asm__ __volatile__("movq %%rsp, %0 \n\t" : "=r"(v0) : :);
    return v0;
}

inline static void cpu_pause()
{
    __asm__ __volatile__("pause \n\t" : : :);
    return;
}

/// print kernel stack (when panic)
void *print_stack(const regs_t *regs, int max_depth);

template <typename T> static inline bool is_user_space_pointer(T ptr)
{
    return (u64)ptr >= memory::minimum_user_addr && (u64)ptr <= memory::maximum_user_addr;
}

template <typename T> static inline bool is_kernel_space_pointer(T ptr)
{
    return (u64)ptr >= memory::minimum_kernel_addr && (u64)ptr <= memory::maximum_kernel_addr;
}