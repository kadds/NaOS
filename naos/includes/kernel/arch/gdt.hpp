#pragma once
#include "common.hpp"
#include "tss.hpp"
namespace arch::gdt
{

struct descriptor
{
    u32 _0;
    u8 _1;
    u8 type : 3;
    u8 S : 1;
    u8 DPL : 2;
    u8 P : 1;
    u8 : 0;
    u8 _4 : 4;
    u8 L : 1;
    u8 DB : 1;
    u8 _3;
    descriptor(u64 gdte) { *((u64 *)this) = gdte; }
    descriptor() {}
} PackStruct;
struct ptr_t
{
    u16 limit;
    u64 addr;
} PackStruct;
enum class selector_type
{
    kernel_code = 0x1,
    kernel_data = 0x2,
    user_code = 0x3,
    user_data = 0x4
};
enum class dpl
{
    kernel,
    user,
};

u16 constexpr gen_selector(selector_type index, int dpl) { return (u16)index << 3 | (dpl & 0x2); }

void init_before_paging();
void init_after_paging();
descriptor &get_gdt_descriptor(int index);
::arch::tss::descriptor &get_tss_descriptor(int index);
int get_offset_of_tss();
} // namespace arch::gdt
