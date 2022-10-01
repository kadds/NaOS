#pragma once
#include "common.hpp"

/**
 *  User space memory zone
0x0000,0000,0000,0000 - 0x0000,0000,001F,FFFF: resevered (1MB)
0x0000,0000,00Y0,0000 - 0x0000,0000,XXXX,XXXX: text
0x0000,0000,XXXX,XXXX - 0x0000,000X,XXXX,XXXX: head
0x0000,000X,XXXX,XXXX - 0x0000,7FFF,EFFF,FFFF: mmap zone
0x0000,7FFF,F000,0000 - 0x0000,8000,0000,0000: resevered (256MB)

  Kernel space memory zone
0xFFFF,8000,0000,0000 - 0xFFFF,9FFF,FFFF,FFFF: fixed phycal memory map (32TB)
0xFFFF,A001,0000,0000 - 0xFFFF,A001,FFFF,FFFF: io mmap address (4GB)

0xFFFF,B000,0000,0000 - 0xFFFF,CFFF,FFFF,FFFF: kernel mmap zone (32TB)
0xFFFF,F000,0000,0000 - 0xFFFF,F000,3FFF,FFFF: Kernel cpu stack memroy map (1GB)
*/
namespace memory
{
constexpr u64 maximum_user_addr = 0x0000'7FFF'FFFF'FFFFUL;
constexpr u64 minimum_user_addr = 0UL;
constexpr u64 minimum_kernel_addr = 0xFFFF'8000'0000'0000UL;
constexpr u64 maximum_kernel_addr = 0xFFFF'FFFF'FFFF'FFFFUL;

constexpr u64 max_memory_support = memory_size_tb(1);

constexpr u64 page_size = 0x1000;
constexpr u64 large_page_size = 0x20'0000;

constexpr u64 fixed_memory_bottom_address = 0xFFFF'8000'0000'0000UL;
constexpr u64 fixed_memory_top_address = 0xFFFF'A000'0000'0000UL;

constexpr u64 io_map_bottom_address = 0xFFFF'A001'0000'0000UL;
constexpr u64 io_map_top_address = 0xFFFF'A002'0000'0000UL;

constexpr u64 kernel_mmap_bottom_address = 0xFFFF'B000'0000'0000UL;
constexpr u64 kernel_mmap_top_address = 0xFFFF'D000'0000'0000UL;
constexpr u64 kernel_cpu_stack_bottom_address = 0xFFFF'F000'0000'0000UL;
constexpr u64 kernel_cpu_stack_top_address = 0xFFFF'F000'4000'0000UL;

constexpr u64 fixed_memory_available_top_address = fixed_memory_bottom_address + max_memory_support;

// initial size 4MB
constexpr u64 user_stack_size = memory_size_mb(4);
// maximum stack size: 256MB
constexpr u64 user_stack_maximum_size = memory_size_mb(256);

constexpr u64 user_mmap_top_address = 0x0000'8000'0000'0000UL;
constexpr u64 user_code_bottom_address = 0x20'0000UL;
/// 4G
constexpr u64 user_head_size = memory_size_gb(4);

// 16 kb stack size in X86_64 arch
constexpr int kernel_stack_page_count = 4;
// 16 kb irq stack size
constexpr int interrupt_stack_page_count = 4;
// 8 kb exception stack size
constexpr int exception_stack_page_count = 2;
// 16 kb exception nmi stack size
constexpr int exception_nmi_stack_page_count = 4;

constexpr u64 kernel_stack_size = kernel_stack_page_count * page_size;
constexpr u64 interrupt_stack_size = interrupt_stack_page_count * page_size;
constexpr u64 exception_stack_size = exception_stack_page_count * page_size;
constexpr u64 exception_nmi_stack_size = exception_nmi_stack_page_count * page_size;

constexpr u64 linear_addr_offset = fixed_memory_bottom_address;

constexpr u64 kernel_cpu_stack_rsp0 = kernel_cpu_stack_bottom_address + page_size + kernel_stack_size;

} // namespace memory
