#pragma once
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/types.hpp"
namespace arch::ACPI
{
void init();

bool has_init();

void enable();

typedef irq::request_result (*timer_fn)(u64 user_data);

bool register_timer(timer_fn, u64 user_data);
void unregister_timer();

// return base address and global irq base
freelibcxx::vector<freelibcxx::tuple<phy_addr_t, u32>> get_io_apic_list();

struct override_irq
{
    u8 bus;
    u8 irq_source;
    u16 flags;
    u32 gsi;
};

freelibcxx::vector<override_irq> get_override_irq_list();

struct pm_info
{
    u32 block_base;
    void *xblock_base;
    bool use_io;
    bool bit32mode;
    u32 event_a;
    u32 xevent_a;
};

freelibcxx::optional<pm_info> get_acpipm_base();

phy_addr_t get_local_apic_base();

freelibcxx::optional<phy_addr_t> get_hpet_base();

} // namespace arch::ACPI
