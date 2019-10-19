#include "kernel/arch/com.hpp"
#include "kernel/arch/io.hpp"
namespace arch::device::com
{
const io_port base_port = 0x3f8;
void init()
{
    io_out8(base_port + 1, 0);
    io_out8(base_port + 3, 0x80);
    io_out8(base_port + 0, 0x1); // divisor  115200
    io_out8(base_port + 1, 0x0); //
    io_out8(base_port + 3, 0x03);
}
void write(byte data)
{
    while ((io_in8(base_port + 5) & 0x20) == 0)
        ;

    io_out8(base_port, (u8)data);
}
void write(const byte *data, u64 len)
{
    for (u64 i = 0; i < len; i++)
    {
        write(data[i]);
    }
}

} // namespace arch::device::com
