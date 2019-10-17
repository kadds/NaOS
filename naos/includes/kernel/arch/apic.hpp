#pragma once
#include "common.hpp"
#include "kernel/arch/io_apic.hpp"
namespace arch::APIC
{
void init();
void EOI(u8 index);
void is_bsp();



} // namespace arch::APIC
