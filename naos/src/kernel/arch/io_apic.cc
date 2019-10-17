#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/trace.hpp"
namespace arch::APIC
{

struct io_map_t
{
    void *base_addr;
    void *index_address;
    void *data_address;
    void *EOI_address;
};

io_map_t io_map;

u32 io_read_register_32(u16 index)
{
    u32 rt;
    *(u16 *)io_map.index_address = index;
    _mfence();
    rt = *(u32 *)io_map.data_address;
    _mfence();
    return rt;
}

void io_write_register_32(u16 index, u32 v)
{
    *(u16 *)io_map.index_address = index;
    _mfence();
    *(u32 *)io_map.data_address = v;
    _mfence();
}

u64 io_read_register(u16 index)
{
    u64 rt;
    rt = io_read_register_32(index + 1);
    rt <<= 32;
    rt |= io_read_register_32(index);
    return rt;
}

void io_write_register(u16 index, u64 value)
{
    io_write_register_32(index, value);
    value >>= 32;
    io_write_register_32(index + 1, value);
}

void io_init()
{

    io_map.base_addr = (void *)(u64)0xfec00000;
    io_map.index_address = memory::kernel_phyaddr_to_virtaddr(io_map.base_addr);
    io_map.data_address = (char *)io_map.index_address + 0x10;
    io_map.EOI_address = (char *)io_map.index_address + 0x40;

    // ID
    io_write_register_32(0, 0x0f000000);
    u8 rte = (io_read_register_32(0x1) >> 16) & 0xFF;
    rte++;
    trace::debug("IO/APIC ID ", (io_read_register_32(0) >> 24) & 0xFF, ", Version ", (io_read_register_32(0x1) & 0xFF),
                 ", RTE count ", rte);

    for (u8 i = 0; i < rte; i++)
    {
        io_write_register(0x10 + i * 2, 0x10020 + i);
    }
    io_irq_setup(2, 0x20, 0b00000000, 0);
    io_enable(2);

    _mfence();
    // APIC mode
    // enable IMCR
    io_out8(0x22, 0x70);
    io_out8(0x23, 0x1);

    io_out32(0xcf8, 0x80000000 | (0 << 16) | (31 << 11) | (0 << 8) | 0xF0);
    _mfence();
    u32 rcba = io_in32(0xcfc);
    u32 rcba_address = rcba & 0xFFFFC000;
    u16 *oic = (u16 *)((u64)rcba_address + 0x31fe);

    trace::debug("RCBA register value ", (void *)(u64)rcba, ", Base address ", (void *)(u64)rcba_address,
                 ", OIC address ", (void *)oic);
    oic = memory::kernel_phyaddr_to_virtaddr(oic);
    u16 v = ((*oic) & 0x7FF) | (1 << 8);
    _mfence();
    *oic = v;
    _mfence();

    // kassert(*oic == v, "OIC register can't write value ", (void *)(u64)v, ", old value", (void *)(u64)((*oic) &
    // 0x7FF));
}

void io_disable(u8 index) { io_write_register(0x10 + index * 2, io_read_register(0x10 + index * 2) | (1 << 16)); }

void io_irq_setup(u8 index, u8 vector, u8 flags, u8 destination_field)
{
    io_write_register(0x10 + index * 0x2, vector | ((u16)flags << 8) | ((u64)destination_field << 56));
}

void io_enable(u8 index) { io_write_register(0x10 + index * 0x2, io_read_register(0x10 + index * 0x2) & ~(1 << 16)); }

void io_EOI(u8 index) { *(u32 *)io_map.EOI_address = 2; }

} // namespace arch::APIC