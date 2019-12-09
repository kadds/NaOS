#pragma once
#include "common.hpp"

namespace arch::device::com
{
class serial
{
    u16 port;

  public:
    void init(u32 port);
    void write(byte data);
    void write(const byte *data, u64 len);
    u64 read(byte *data, u64 len);
    byte read();
};

u32 select_port(int index);

} // namespace arch::device::com
