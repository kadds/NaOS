#pragma once
#include "kernel/common.hpp"
#include "kernel/iommu/backend.hpp"
namespace arch::APIC
{

struct io_entry
{
    bool is_disable : 1 = true;
    bool is_level_trigger_mode : 1 = false;
    bool low_level_polarity : 1 = false;
    bool is_logic_mode : 1 = false;
    bool is_remapped : 1 = false;
    iommu::interrupt_route_token interrupt_route{};
    u64 remapped_redirection_entry = 0;

    enum class mode_t : u8
    {
        fixed = 0,
        lower = 1,
        smi = 2,
        nmi = 4,
        init = 5,
        extInt = 7,
    } delivery_mode = mode_t::fixed;

    u8 dest_apic_id = 0;

    io_entry &operator=(const io_entry &io)
    {
        is_disable = io.is_disable;
        is_level_trigger_mode = io.is_level_trigger_mode;
        is_logic_mode = io.is_logic_mode;
        low_level_polarity = io.low_level_polarity;
        delivery_mode = io.delivery_mode;
        dest_apic_id = io.dest_apic_id;
        is_remapped = io.is_remapped;
        interrupt_route = io.interrupt_route;
        remapped_redirection_entry = io.remapped_redirection_entry;
        return *this;
    }
};

void io_init();

bool io_enable(u8 index);
bool io_disable(u8 index);
u8 sci_vector();
u32 sci_gsi();

u8 io_irq_setup(u32 gsi, io_entry *entry);
bool enable_interrupt_remapping_if_safe();
/// Send directed EOI only when the vector is registered to an IOAPIC pin.
void io_EOI(u8 vector);

enum class gsi_vector
{
    pit = 0,
    hpet = 1,
};

u32 query_gsi(gsi_vector vec);
void set_gsi(gsi_vector, u32 gsi);
bool exist(gsi_vector vec);

} // namespace arch::APIC
