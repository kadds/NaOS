#pragma once
#include "common.hpp"
struct idt_entry
{
    u16 offset0;
    u16 selector;
    u8 lst : 2;
    u8 : 0;
    u8 type : 3;
    u8 _0 : 1;
    u8 DPL : 2;
    u8 P : 1;
    u16 offset1;
    u32 offset2;
    u32 zero;
    u64 offset() { return (u64)offset2 << 32 | (u64)offset1 << 16 | (u64)offset0; }
    void set_offset(u64 offset)
    {
        offset2 = offset >> 32;
        offset1 = offset >> 16;
        offset0 = offset;
    }
};
struct idt_ptr
{
    u16 limit;
    u64 addr;
} PackStruct;

class idt
{
  private:
  public:
    static void init_before_paging();
    static void init_after_paging();
};