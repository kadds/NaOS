#pragma once
#include "common.hpp"

namespace arch::device::com
{
void init();
void write(byte data);
void write(const byte *data, u64 len);

} // namespace arch::device::com
