#pragma once
#include "common.hpp"

ExportC void _load_gdt(void *gdt);
ExportC void _load_idt(void *idt);
ExportC void _load_tss_descriptor(void *tss);
ExportC void _load_page(void *page_addr);
ExportC void _reload_segment(u64 cs, u64 ss);
ExportC void _cli();
ExportC void _sti();
ExportC u64 _get_cr2();
// return page addr
ExportC u64 _flush_tlb();

ExportC void _unpaged_load_gdt(void *gdt);
ExportC void _unpaged_load_idt(void *idt);
ExportC void _unpaged_load_page(void *page_addr);
ExportC void _unpaged_reload_segment(u64 cs, u64 ss);
ExportC void _unpaged_reload_kernel_virtual_address(u64 offset);
