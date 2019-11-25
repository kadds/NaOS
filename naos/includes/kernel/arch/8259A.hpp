#pragma once
#include "../kernel.hpp"

namespace arch::device::chip8259A
{
void init();
void enable_all();
void disable_all();
// Set which bit you want irq to enable. bit 1 is able a irq
void enable_with(u8 ports);
// Set which bit you want irq to enable. bit 1 is disable a irq
void disable_with(u8 ports);

void _ctx_interrupt_ send_EOI(int irq_number);
} // namespace arch::device::chip8259A
