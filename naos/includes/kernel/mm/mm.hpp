#pragma once
#include "common.hpp"

/* User space memory zone
0x0000,0000,0000,0000 - 0x0000,0000,003F,FFFF: resevered (256MB)
0x0000,0000,0040,0000 - 0x0000,0000,XXXX,XXXX: text
0x0000,0000,XXXX,XXXX - 0x0000,000X,XXXX,XXXX: head
0x0000,000X,XXXX,XXXX - 0x0000,7FFF,EFFF,FFFF: mmap zone
0x0000,7FFF,F000,0000 - 0x0000,8000,0000,0000: resevered (256MB)
*/
/* Kernel space memory zone
0xFFFF,8000,0000,0000 - 0xFFFF,8000,FFFF,FFFF: fixed phycal memory map
0xFFFF,a000,0000,0000 - 0xFFFF,a000,0FFF,FFFF: vga map (64MB)
0xFFFF,a000,1000,0000 - 0xFFFF,a000,1FFF,FFFF: IO map address (UC)
0xFFFF,b000,0000,0000 - 0xFFFF,cFFF,FFFF,FFFF: kernel mmap zone (32T)
*/
namespace memory
{

constexpr u64 io_map_start_address = 0xFFFFa00010000000UL;
constexpr u64 io_map_end_address = 0xFFFFa00020000000UL;

// kernel stack size in task
extern const u64 kernel_stack_size;

extern const u64 interrupt_stack_size;

extern const u64 exception_stack_size;

extern const u64 exception_nmi_stack_size;

// stack size: 4MB
extern const u64 user_stack_size;
// maximum stack size: 256MB
extern const u64 user_stack_maximum_size;

extern const u64 user_mmap_top_address;

extern const u64 user_code_bottom_address;

extern const u64 user_head_size;

extern const u64 kernel_mmap_top_address;

extern const u64 kernel_mmap_bottom_address;

extern const u64 kernel_vga_bottom_address;

extern const u64 kernel_vga_top_address;

extern const u64 linear_addr_offset;

// 16 kb stack size in X86_64 arch
extern const int kernel_stack_page_count;
// 16 kb irq stack size
extern const int interrupt_stack_page_count;
// 8 kb exception stack size
extern const int exception_stack_page_count;
// 8 kb exception extra stack size
extern const int exception_ext_stack_page_count;
// 16 kb soft irq stack size
extern const int soft_irq_stack_page_count;

extern const u64 max_memory_support;

extern const u64 linear_addr_offset;

extern const u64 page_size;

} // namespace memory
