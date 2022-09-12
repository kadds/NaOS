#include "kernel/arch/io_apic.hpp"
#include "common.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/trace.hpp"
namespace arch::APIC
{

class io_map_t
{
  public:
    io_map_t(phy_addr_t base, int irq_base)
        : base_phy_(base)
        , irq_base_(irq_base)
        , io_entry_(memory::MemoryAllocatorV)
    {
    }

    void update_base_addr(void *base)
    {
        index_addr_ = base;
        data_addr_ = (char *)index_addr_ + 0x10;
        EOI_addr_ = (char *)index_addr_ + 0x40;
    }

    void update_info(u8 count, u8 id, u8 version)
    {
        rte_count_ = count;
        id_ = id;
        version_ = version;
        io_entry_.resize(count, {});

        // disable all
        for (u8 i = 0; i < count; i++)
        {
            io_write_register(0x10 + i * 2, 0x10020 + i);
        }
    }

    u32 io_read_register_32(u16 index)
    {
        u32 rt;
        *(volatile u16 *)index_addr_ = index;
        _mfence();
        rt = *(volatile u32 *)data_addr_;
        _mfence();
        return rt;
    }
    void io_write_register_32(u16 index, u32 v)
    {
        *(volatile u16 *)index_addr_ = index;
        _mfence();
        *(volatile u32 *)data_addr_ = v;
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

    phy_addr_t base_address() const { return base_phy_; }

    u8 irq_setup(u8 index, io_entry *entry)
    {
        kassert(index < io_entry_.size(), "index check fail");
        u32 flags = 0;
        if (entry->is_disable)
            flags |= 1U << 16;
        if (entry->is_level_trigger_mode)
            flags |= 1U << 15;
        if (entry->is_logic_mode)
            flags |= 1U << 11;
        if (entry->low_level_polarity)
            flags |= 1U << 13;

        flags |= ((u16)entry->delivery_mode) << 8;

        io_entry_[index] = *entry;
        u8 vec = index + 0x20;
        u64 data = vec | flags | (((u64)entry->dest_apic_id) << 56);
        io_write_register(0x10 + index * 0x2, data);

        return vec;
    }

    u8 irq_base() const { return irq_base_; }
    u8 rte_count() const { return rte_count_; }

    void enable(u8 index)
    {
        kassert(index < io_entry_.size(), "index check fail");
        io_write_register(0x10 + index * 0x2, io_read_register(0x10 + index * 0x2) & ~(1UL << 16));
    }

    void disable(u8 index)
    {
        kassert(index < io_entry_.size(), "index check fail");
        io_write_register(0x10 + index * 2, io_read_register(0x10 + index * 2) | (1UL << 16));
    }

    void eoi(u8 index)
    {
        kassert(index < io_entry_.size(), "index check fail");
        if (io_entry_[index].is_level_trigger_mode)
        {
            *(u32 *)EOI_addr_ = index;
        }
    }

  private:
    phy_addr_t base_phy_;
    void *index_addr_;
    void *data_addr_;
    void *EOI_addr_;
    u8 irq_base_;
    u8 rte_count_;
    u8 id_;
    u8 version_;
    freelibcxx::vector<io_entry> io_entry_;
};

freelibcxx::vector<io_map_t> *io_map_vec;
u32 pit_gsi = 0;
u32 sci_gsi = 9;

void io_init()
{
    trace::debug("IO APIC init");
    bool acpi = cmdline::get_bool("acpi", false);
    io_map_vec = memory::MemoryAllocatorV->New<freelibcxx::vector<io_map_t>>(memory::MemoryAllocatorV);

    if (acpi)
    {
        for (auto [addr, offset] : arch::ACPI::get_io_apic_list())
        {
            io_map_t io_map(addr, offset);
            io_map_vec->push_back(io_map);
        }
    }
    else
    {
        io_map_vec->push_back(phy_addr_t::from((void *)0xFEC0'0000UL), 0);
    }

    int i = 0;

    for (auto &io_map : *io_map_vec)
    {
        u64 map_base = memory::alloc_io_mmap_address(paging::frame_size::size_2mb, paging::frame_size::size_2mb);
        auto &paging = memory::kernel_vm_info->paging();
        paging.big_page_map_to(reinterpret_cast<void *>(map_base), paging::big_pages, io_map.base_address(),
                               paging::flags::writable | paging::flags::cache_disable | paging::flags::write_through,
                               0);

        paging.reload();
        io_map.update_base_addr(reinterpret_cast<void *>(map_base));

        // ID
        io_map.io_write_register_32(0, 0x0f000000);
        u8 rte = (io_map.io_read_register_32(0x1) >> 16) & 0xFF;
        u8 id = (io_map.io_read_register_32(0) >> 24) & 0xFF;
        u8 version = (io_map.io_read_register_32(0x1)) & 0xFF;
        rte++;

        trace::debug("IO APIC ", i, " base ", trace::hex(io_map.base_address()()), " id ", id, " version ", version,
                     " RTE count ", rte);
        if (rte == 1)
        {
            trace::warning("RTE count maybe invalid");
        }
        io_map.update_info(rte, id, version);
    }

    // init pit timer
    io_entry pit_entry;
    io_entry sci_entry; // System Control Interrupts

    if (acpi)
    {
        for (auto &override_irq : ACPI::get_override_irq_list())
        {
            trace::debug("IRQ override from ", override_irq.irq_source, " to ", override_irq.gsi, " bus ",
                         override_irq.bus, " flags ", override_irq.flags);
            io_entry *entry = nullptr;
            if (override_irq.irq_source == 0)
            {
                // pit timer
                entry = &pit_entry;
                pit_gsi = override_irq.gsi;
            }
            else if (override_irq.irq_source == 9)
            {
                entry = &sci_entry;
                sci_gsi = override_irq.gsi;
            }
            if (entry)
            {
                if (override_irq.flags & 0x2)
                {
                    entry->low_level_polarity = true;
                }
                if (override_irq.flags & 0x8)
                {
                    entry->is_level_trigger_mode = true;
                }
            }
        }
    }
    else
    {
        pit_gsi = 2;
        sci_gsi = 9;
    }
    if (pit_gsi > 0)
    {
        io_irq_setup(pit_gsi, &pit_entry);
    }

    io_irq_setup(sci_gsi, &sci_entry);
    arch::APIC::io_enable(sci_gsi);
}

u32 hpet_gsi = 2;

u32 query_gsi(gsi_vector vec)
{
    switch (vec)
    {
        case gsi_vector::pit:
            return pit_gsi;
        case gsi_vector::hpet:
            return hpet_gsi;
        default:
            trace::panic("invalid gsi ", (int)vec);
    }
    return 0;
}
bool exist(gsi_vector vec)
{
    switch (vec)
    {
        case gsi_vector::pit:
            return pit_gsi != 0;
        case gsi_vector::hpet:
            return true;
        default:
            return false;
    }
}

void set_gsi(gsi_vector vec, u32 gsi)
{
    if (vec == gsi_vector::hpet)
    {
        hpet_gsi = gsi;
    }
}

io_map_t *which(u8 index)
{
    for (auto &io_map : *io_map_vec)
    {
        if (io_map.irq_base() <= index && io_map.irq_base() + io_map.rte_count() > index)
        {
            return &io_map;
        }
    }
    return nullptr;
}

void io_enable(u8 index)
{
    auto io = which(index);
    kassert(io, "io index not found ", index);
    return io->enable(index - io->irq_base());
}

void io_disable(u8 index)
{
    auto io = which(index);
    kassert(io, "io index not found ", index);
    return io->disable(index - io->irq_base());
}

u8 io_irq_setup(u8 index, io_entry *entry)
{
    auto io = which(index);
    kassert(io, "io index not found ", index);
    return io->irq_setup(index - io->irq_base(), entry);
}

void io_EOI(u8 index)
{
    auto io = which(index);
    kassert(io, "io index not found ", index);
    return io->eoi(index - io->irq_base());
}
} // namespace arch::APIC