#include "kernel/arch/com.hpp"
#include "kernel/arch/io.hpp"
namespace arch::device::com
{
const io_port base_control_port[] = {0x3f8, 0x2f8, 0x3e8, 0x2e8};

void io_write(io_port base_port, byte data)
{
    while ((io_in8(base_port + 5) & 0x20) == 0)
        ;

    io_out8(base_port, (u8)data);
}

byte io_read(io_port base_port)
{
    while ((io_in8(base_port + 5) & 0x1) == 0)
        ;

    return (byte)io_in8(base_port);
}

u16 get_control_port(int index) { return base_control_port[index]; }

void serial::init(u16 control_port)
{
    this->port = control_port;

    io_out8(port + 1, 0);
    io_out8(port + 3, 0x80);
    io_out8(port + 0, 0x1); // divisor  115200
    io_out8(port + 1, 0x0); //
    io_out8(port + 3, 0x03);
}

void serial::write(byte data) { io_write(port, data); }

void serial::write(const byte *data, u64 len)
{
    for (u64 i = 0; i < len; i++)
    {
        io_write(port, data[i]);
    }
}

u64 serial::read(byte *data, u64 len)
{
    for (u64 i = 0; i < len; i++)
    {
        data[i] = io_read(port);
    }
    return len;
}

byte serial::read() { return io_read(port); }

} // namespace arch::device::com
