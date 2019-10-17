#pragma once
#include "common.hpp"
#include "idt.hpp"

namespace arch::interrupt
{

void init();
void set_callback(arch::idt::call_func func);
void set_soft_irq_callback(arch::idt::call_func func);

} // namespace arch::interrupt
