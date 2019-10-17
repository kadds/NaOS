#include "kernel/arch/8259A.hpp"
#include "kernel/arch/io.hpp"

namespace arch::device::chip8259A
{
const int master_icw1_port = 0x20;
const int master_icw2_port = 0x21;
const int master_icw3_port = 0x21;
const int master_icw4_port = 0x21;

const int master_ocw1_port = 0x21;
const int master_ocw2_port = 0x20;
const int master_ocw3_port = 0x20;

const int slave_icw1_port = 0xA0;
const int slave_icw2_port = 0xA1;
const int slave_icw3_port = 0xA1;
const int slave_icw4_port = 0xA1;

const int slave_ocw1_port = 0xA1;
const int slave_ocw2_port = 0xA0;
const int slave_ocw3_port = 0xA0;

void init()
{
    // master
    io_out8(master_icw1_port, 0x11);
    io_out8(master_icw2_port, 0x20);
    io_out8(master_icw3_port, 0x04);
    io_out8(master_icw4_port, 0x01);

    // slave
    io_out8(slave_icw1_port, 0x11);
    io_out8(slave_icw2_port, 0x28);
    io_out8(slave_icw3_port, 0x02);
    io_out8(slave_icw4_port, 0x01);

    disable_all();
}

void enable_all()
{
    io_out8(master_ocw1_port, 0x00);
    io_out8(slave_ocw1_port, 0x00);
}

void disable_all()
{
    io_out8(master_ocw1_port, 0xFF);
    io_out8(slave_ocw1_port, 0xFF);
}

void enable_with(u8 ports)
{
    u8 p = io_in8(master_ocw1_port);
    p = p & ~(1 << ports);
    io_out8(master_ocw1_port, p);
    p = io_in8(slave_ocw1_port);
    p = p & ~(1 << ports);
    io_out8(slave_ocw1_port, p);
}

void disable_with(u8 ports)
{
    u8 p = io_in8(master_ocw1_port);
    p = p | (1 << ports);
    io_out8(master_ocw1_port, p);
    p = io_in8(slave_ocw1_port);
    p = p | (1 << ports);
    io_out8(slave_ocw1_port, p);
}

void _ctx_interrupt_ send_EOI(int irq_number)
{
    if (unlikely(irq_number > 16))
        return;
    io_out8(master_ocw2_port, 0x20);
    if (irq_number > 8)
    {
        io_out8(slave_ocw2_port, 0x20);
    }
}
} // namespace arch::device::chip8259A
