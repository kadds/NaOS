#pragma once
#include "common.hpp"

namespace idt
{
struct regs
{
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 ds;
    u64 es;
    u64 func;
    u64 rax;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};
struct ptr_t
{
    u16 limit;
    u64 addr;
} PackStruct;
struct entry
{
    u16 offset0;
    u16 selector;
    u8 ist : 3;
    u8 : 0;
    u8 type : 4;
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
    void set_type(u8 type) { this->type = type; }
    void set_dpl(u8 dpl) { this->DPL = dpl; }
    void set_valid(bool p) { this->P = p; }
    void set_ist(u8 ist) { this->ist = ist; }
    void set_selector(u16 selector) { this->selector = selector; }
    entry()
    {
        *(u64 *)this = 0;
        *((u64 *)this + 1) = 0;
    }
};
void enable();
void disable();
void init_before_paging();
void init_after_paging();
void set_entry(int id, void *func, u16 selector, u8 dpl, u8 present, u8 type, u8 ist);
void set_exception_entry(int id, void *function, u16 selector, u8 dpl, u8 ist);
void set_interrupt_entry(int id, void *function, u16 selector, u8 dpl, u8 ist);
} // namespace idt
