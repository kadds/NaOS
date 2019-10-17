#pragma once
#include "common.hpp"
#include "idt.hpp"

namespace arch::exception
{
void init();
void set_callback(arch::idt::call_func func);
void set_exit_callback(arch::idt::call_func func);

namespace vector
{
const u32 divide_error = 0;
const u32 debug = 1;
const u32 nmi = 2;
const u32 int3 = 3;
const u32 overflow = 4;
const u32 bounds = 5;
const u32 undefined_opcode = 6;
const u32 dev_not_available = 7;
const u32 double_fault = 8;
const u32 coprocessor_segment_overrun = 9;
const u32 invalid_TSS = 10;
const u32 segment_not_present = 11;
const u32 stack_segment_fault = 12;
const u32 general_protection = 13;
const u32 page_fault = 14;
const u32 x87_FPU_error = 16;
const u32 alignment_check = 17;
const u32 machine_check = 18;
const u32 SIMD_exception = 19;
const u32 virtualization_exception = 20;
} // namespace vector
} // namespace arch::exception
