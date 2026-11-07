#pragma once

#include "kernel/types.hpp"

namespace acpi
{

struct fixed_register
{
    u64 address = 0;
    u32 length = 0;
    u8 space = 0; // 0 = SystemMemory, 1 = SystemIO
    u8 gas_bit_offset = 0;
    u8 gas_access_size = 0;
    u8 access_width = 0; // bytes; resolved from GAS or the fixed register extent
};

struct fixed_inventory
{
    u64 boot_epoch = 0;
    u64 firmware_generation = 0;
    u64 facs_global_lock_address = 0; // Kernel-private address of FACS.GlobalLock; never serialized.
    u32 sci_gsi = 0;
    u32 gpe1_base = 0;
    bool hardware_reduced = false;
    fixed_register pm1_event;
    fixed_register pm1_event_enable;
    fixed_register pm1_event_b;
    fixed_register pm1_event_b_enable;
    fixed_register pm1_control;
    fixed_register pm1_control_b;
    fixed_register pm2_control;
    fixed_register gpe0_status;
    fixed_register gpe0_enable;
    fixed_register gpe1_status;
    fixed_register gpe1_enable;
    fixed_register smi_command;
    fixed_register reset;
    fixed_register sleep_control;
    fixed_register sleep_status;
    u8 reset_value = 0;
};

struct fadt_fixed_inventory
{
    static constexpr u32 max_reserved_ranges = 24;

    fixed_inventory active;
    fixed_register reserved[max_reserved_ranges]{};
    u32 reserved_count = 0;
};

/// Parse fixed FADT registers. Extended GAS locations are preferred for
/// operations while all legacy and extended aliases are retained as reserved
/// ranges so generic AML access cannot claim them.
bool parse_fadt_fixed_inventory(const u8 *bytes, u64 length, u64 boot_epoch, u64 firmware_generation,
                                fadt_fixed_inventory &result);

} // namespace acpi
