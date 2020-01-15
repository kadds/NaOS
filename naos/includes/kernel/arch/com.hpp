#pragma once
#include "common.hpp"

namespace arch::device::com
{
class serial
{
    /// base serial port
    u16 port;

  public:
    void init(u16 port);
    void write(byte data);
    void write(const byte *data, u64 len);
    /// read bytes to buffer
    u64 read(byte *data, u64 len);
    /// read one byte
    byte read();
};

u16 select_port(int index);

} // namespace arch::device::com
