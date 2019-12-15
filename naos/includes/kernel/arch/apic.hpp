#pragma once
#include "common.hpp"
namespace arch::APIC
{
void init();

///
/// \brief send eoi to apic
///
/// \param index interrupt vector
///
void EOI(u8 index);

} // namespace arch::APIC
