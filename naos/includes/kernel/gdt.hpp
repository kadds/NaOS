#pragma once
#include "common.hpp"

struct gdt_entry
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
    gdt_entry(u64 gdte) { *((u64 *)this) = gdte; }
} PackStruct;

struct gdt_ptr
{
    u16 limit;
    u64 addr;
} PackStruct;

class gdt
{
  private:
  public:
    static void init_before_paging();
    static void init_after_paging();
};
