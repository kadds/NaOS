#pragma once
#include "common.hpp"
namespace arch::APIC
{
void io_init();

void io_enable(u8 index);
void io_irq_setup(u8 index, u8 vector, u8 flags, u8 destination_field);
void io_disable(u8 index);
void io_EOI(u8 index);
} // namespace arch::APIC
