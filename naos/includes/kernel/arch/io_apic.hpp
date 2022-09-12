#pragma once
#include "common.hpp"
namespace arch::APIC
{

struct io_entry
{
    bool is_disable : 1 = true;
    bool is_level_trigger_mode : 1 = false;
    bool low_level_polarity : 1 = false;
    bool is_logic_mode : 1 = false;

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
        return *this;
    }
};

void io_init();

void io_enable(u8 index);
void io_disable(u8 index);

u8 io_irq_setup(u8 index, io_entry *entry);
void io_EOI(u8 index);

enum class gsi_vector
{
    pit = 0,
    hpet = 1,
};

u32 query_gsi(gsi_vector vec);
void set_gsi(gsi_vector, u32 gsi);
bool exist(gsi_vector vec);

} // namespace arch::APIC
