#pragma once
#include "common.hpp"

namespace arch::tss
{

struct descriptor
{
    u16 limit0;
    u16 offset0;
    u8 offset1;
    u8 type : 4;
    u8 system : 1;
    u8 DPL : 2;
    u8 P : 1;
    u8 limit1 : 4;
    u8 AVL : 1;
    u8 : 0;
    u8 offset2;
    u32 offset3;
    descriptor()
    {
        *((u64 *)this) = 0;
        *((u64 *)this + 1) = 0;
    }
    u64 offset() { return ((u64)offset3) << 32 | ((u64)offset2 << 24) | ((u64)offset1) << 16 | (u64)offset0; }
    void set_offset(u64 offset)
    {
        offset3 = offset >> 32;
        offset2 = offset >> 24;
        offset1 = offset >> 16;
        offset0 = offset;
    }

    u32 limit() { return ((u32)limit0) | ((u32)limit1) << 16; }
    void set_limit(u32 limit)
    {
        limit0 = limit;
        limit1 = limit >> 16;
    }
    void set_type(u8 type) { this->type = type; }
    void set_dpl(u8 dpl) { this->DPL = dpl; }
    void set_valid(bool p) { this->P = p; }
} PackStruct;

struct tss_t
{
    u32 _0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 _1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 _2;
    u16 _3;
    u16 io_address;
} PackStruct;

void init(void *baseAddr, void *ist);

void set_ist(int index, void *ist);
void *get_ist(int index);

void set_rsp(int index, void *rsp0);
void *get_rsp(int index);
} // namespace arch::tss
