#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
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
    *(volatile u16 *)io_map.index_address = index;
    _mfence();
    rt = *(volatile u32 *)io_map.data_address;
    _mfence();
    return rt;
}

void io_write_register_32(u16 index, u32 v)
{
    *(volatile u16 *)io_map.index_address = index;
    _mfence();
    *(volatile u32 *)io_map.data_address = v;
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
io_entry *all_entry;
u8 rte_count;

void io_init()
{
    io_map.base_addr = (void *)(u64)0xfec00000;

    u64 start = memory::io_apic_bottom_address;

    paging::map((paging::base_paging_t *)memory::kernel_vm_info->mmu_paging.get_base_page(), (void *)start,
                phy_addr_t::from(io_map.base_addr), paging::frame_size::size_2mb, 1,
                paging::flags::uncacheable | paging::flags::writable | paging::flags::write_through, false);

    paging::reload();

    io_map.base_addr = (void *)start;

    io_map.index_address = io_map.base_addr;
    io_map.data_address = (char *)io_map.index_address + 0x10;
    io_map.EOI_address = (char *)io_map.index_address + 0x40;
    // ID
    io_write_register_32(0, 0x0f000000);
    u8 rte = (io_read_register_32(0x1) >> 16) & 0xFF;
    rte++;
    trace::debug("IO/APIC ID ", (io_read_register_32(0) >> 24) & 0xFF, ", Version ", (io_read_register_32(0x1) & 0xFF),
                 ", RTE count ", rte);
    if (rte == 1)
    {
        trace::warning("RTE count maybe invalid");
    }
    rte_count = rte;
    all_entry = memory::NewArray<io_entry>(memory::KernelCommonAllocatorV, rte);
    for (u8 i = 0; i < rte; i++)
    {
        io_write_register(0x10 + i * 2, 0x10020 + i);
    }
    io_entry entry;
    entry.dest_apic_id = 0;
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = false;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;
    io_irq_setup(2, &entry);

    _mfence();
    // APIC mode
    // enable IMCR
    io_out8(0x22, 0x70);
    io_out8(0x23, 0x1);

    io_out32(0xcf8, 0x80000000 | (0 << 16) | (31 << 11) | (0 << 8) | 0xF0);
    _mfence();
    u32 rcba = io_in32(0xcfc);
    phy_addr_t rcba_address = phy_addr_t::from(rcba & 0xFFFFC000);
    phy_addr_t base_addr = memory::align_down(rcba_address, paging::frame_size::size_2mb);

    start = memory::rcba_apic_bottom_address;
    paging::map((paging::base_paging_t *)memory::kernel_vm_info->mmu_paging.get_base_page(), (void *)start, base_addr,
                paging::frame_size::size_2mb, 1, paging::flags::uncacheable | paging::flags::writable, false);

    paging::reload();

    trace::debug("RCBA register value ", (void *)(u64)rcba, ", Base address ",
                 reinterpret_cast<addr_t>(rcba_address()));
    if (!(rcba & 0x1))
    {
        trace::panic("RCBA isn't an enable address");
    }

    u8 *oic = (u8 *)(start + 0x31FF + (rcba_address - base_addr));
    u8 v = *oic;
    v |= 1;
    *oic = v;
    // kassert(*oic == v, "OIC register can't write value ", (void *)(u64)v, ", old value", (void *)(u64)(*oic));
}

void io_disable(u8 index)
{
    kassert(index < rte_count, "index check fail");
    io_write_register(0x10 + index * 2, io_read_register(0x10 + index * 2) | (1 << 16));
}

u8 io_irq_setup(u8 index, io_entry *entry)
{
    kassert(index < rte_count, "index check fail");
    u32 flags = 0;
    if (entry->is_disable)
        flags |= 1 << 16;
    if (entry->is_level_trigger_mode)
        flags |= 1 << 15;
    if (entry->is_logic_mode)
        flags |= 1 << 11;
    if (entry->low_level_polarity)
        flags |= 1 << 13;
    flags |= ((u16)entry->delivery_mode) << 8;

    all_entry[index] = *entry;
    u8 vec = index + 0x20;
    u64 data = vec | (flags) | (((u64)entry->dest_apic_id) << 56);
    io_write_register(0x10 + index * 0x2, data);

    return vec;
}

void io_enable(u8 index)
{
    kassert(index < rte_count, "index check fail");
    io_write_register(0x10 + index * 0x2, io_read_register(0x10 + index * 0x2) & ~(1 << 16));
}

void io_EOI(u8 index)
{
    index -= 0x20;
    kassert(index < rte_count, "index check fail");
    if (all_entry[index].is_level_trigger_mode)
        *(u32 *)io_map.EOI_address = index;
}

} // namespace arch::APIC