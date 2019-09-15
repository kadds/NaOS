#pragma once
#include "../kernel.hpp"
#include "common.hpp"
#include "task.hpp"
ExportC void _load_gdt(void *gdt);
ExportC void _load_idt(void *idt);
ExportC void _load_tss_descriptor(int index);
ExportC void _load_page(void *page_addr);
ExportC void _reload_segment(u64 cs, u64 ss);
ExportC void _cli();
ExportC void _sti();
ExportC u64 _get_cr2();
// return page addr
ExportC u64 _flush_tlb();
ExportC void _cpu_id(u64 param, u32 *out_eax, u32 *out_ebx, u32 *out_ecx, u32 *out_edx);
#define cpu_id_ex(eax, ecx, o_eax, o_ebx, o_ecx, o_edx)                                                                \
    _cpu_id((ecx) << 32 & (u32)(eax), (o_eax), (o_ebx), (o_ecx), (o_edx))
#define cpu_id(eax, o_eax, o_ebx, o_ecx, o_edx) _cpu_id((u32)(eax), (o_eax), (o_ebx), (o_ecx), (o_edx))

ExportC NoReturn void _kernel_thread(arch::task::regs_t *regs);
ExportC NoReturn void _switch_task(arch::task::register_info_t *prev, arch::task::register_info_t *next);

ExportC void _unpaged_load_gdt(void *gdt);
ExportC void _unpaged_load_idt(void *idt);
ExportC void _unpaged_load_page(void *page_addr);
ExportC void _unpaged_reload_segment(u64 cs, u64 ss);
ExportC void _unpaged_reload_kernel_virtual_address(u64 offset);

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

ExportC volatile char _sys_call;
ExportC volatile char _sys_ret;

ExportC NoReturn void _call_sys_ret(arch::task::regs_t *regs);
