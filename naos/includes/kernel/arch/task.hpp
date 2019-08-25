#pragma once
#include "common.hpp"
namespace task
{
struct register_info_t
{
    u64 rsp0; // base rsp
    u64 rip;
    u64 rsp;
    u64 fs;
    u64 gs;
    u64 cr2;
    u64 trap_vector;
    u64 error_code;
};
struct regs_t
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
    u64 rax;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};
} // namespace task
