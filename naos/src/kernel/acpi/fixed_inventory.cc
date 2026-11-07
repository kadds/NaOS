#include "kernel/acpi/fixed_inventory.hpp"

namespace acpi
{
namespace
{
constexpr u64 page_size = 4096;

bool read_u32(const u8 *bytes, u64 length, u64 offset, u32 &value)
{
    if (offset > length || sizeof(u32) > length - offset)
        return false;
    value = static_cast<u32>(bytes[offset]) | static_cast<u32>(bytes[offset + 1]) << 8 |
            static_cast<u32>(bytes[offset + 2]) << 16 | static_cast<u32>(bytes[offset + 3]) << 24;
    return true;
}

bool read_u64(const u8 *bytes, u64 length, u64 offset, u64 &value)
{
    u32 low = 0;
    u32 high = 0;
    if (!read_u32(bytes, length, offset, low) || !read_u32(bytes, length, offset + sizeof(u32), high))
        return false;
    value = static_cast<u64>(low) | static_cast<u64>(high) << 32;
    return true;
}

bool valid_range(const fixed_register &reg)
{
    if (reg.address == 0 || reg.length == 0 || reg.address > ~u64{0} - reg.length)
        return false;
    const u64 end = reg.address + reg.length;
    if (reg.space == 1)
        return end <= 0x1'0000;
    return reg.space == 0 && end <= ~u64{0} - (page_size - 1);
}

bool resolve_access_width(fixed_register &reg)
{
    if (reg.address == 0 && reg.length == 0)
    {
        reg.access_width = 0;
        return true;
    }
    if (reg.address == 0 || reg.length == 0 || reg.gas_bit_offset != 0 || reg.gas_access_size > 4)
        return false;

    u32 width = 0;
    if (reg.gas_access_size != 0)
    {
        width = 1u << (reg.gas_access_size - 1);
        if ((reg.space == 1 && width > sizeof(u32)) || reg.length < width || reg.length % width != 0 ||
            reg.address % width != 0)
            return false;
    }
    else
    {
        const u32 maximum_width = reg.space == 1 ? sizeof(u32) : sizeof(u64);
        for (u32 candidate = maximum_width; candidate != 0; candidate >>= 1)
        {
            if (reg.length % candidate == 0 && reg.address % candidate == 0)
            {
                width = candidate;
                break;
            }
        }
    }
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return false;
    reg.access_width = static_cast<u8>(width);
    return true;
}

bool read_gas(const u8 *bytes, u64 length, u64 offset, u32 block_extent, fixed_register &reg)
{
    reg = {};
    if (offset > length || 12 > length - offset)
        return true;

    u64 address = 0;
    if (!read_u64(bytes, length, offset + 4, address))
        return false;
    if (address == 0)
        return true;

    const u8 space = bytes[offset];
    const u8 width = bytes[offset + 1];
    const u8 bit_offset = bytes[offset + 2];
    const u8 access_size = bytes[offset + 3];
    if (space != 0 && space != 1)
        return false;
    if (bit_offset != 0 || access_size > 4 || (width != 0 && (width & 7u) != 0))
        return false;
    u32 extent = block_extent;
    if (extent == 0)
    {
        if (width == 0 || width > 64)
            return false;
        extent = (static_cast<u32>(width) + 7) / 8;
    }
    else if (width != 0 && width > extent * 8)
        return false;

    reg = fixed_register{address, extent, space, bit_offset, access_size, 0};
    if (access_size != 0)
    {
        const u32 access_width = 1u << (access_size - 1);
        if ((space == 1 && access_width > sizeof(u32)) || (block_extent != 0 && access_width > extent))
            return false;
        if (block_extent == 0 && access_width > reg.length)
            reg.length = access_width;
    }
    return valid_range(reg) && resolve_access_width(reg);
}

bool read_legacy(const u8 *bytes, u64 length, u64 offset, u8 extent, fixed_register &reg)
{
    u32 address = 0;
    if (!read_u32(bytes, length, offset, address))
        return false;
    reg = {};
    if (address == 0)
        return true;
    if (extent == 0)
        return false;
    reg = fixed_register{address, extent, 1};
    return valid_range(reg);
}

bool add_reserved(fadt_fixed_inventory &inventory, const fixed_register &reg)
{
    if (reg.address == 0 && reg.length == 0)
        return true;
    if (!valid_range(reg))
        return false;
    for (u32 index = 0; index < inventory.reserved_count; ++index)
    {
        const auto &existing = inventory.reserved[index];
        if (existing.address == reg.address && existing.length == reg.length && existing.space == reg.space)
            return true;
    }
    if (inventory.reserved_count == fadt_fixed_inventory::max_reserved_ranges)
        return false;
    inventory.reserved[inventory.reserved_count++] = reg;
    return true;
}

bool select_block(const u8 *bytes, u64 length, u64 legacy_offset, u8 extent, u64 gas_offset, fixed_register &active,
                  fadt_fixed_inventory &inventory)
{
    fixed_register legacy{};
    fixed_register extended{};
    if (!read_legacy(bytes, length, legacy_offset, extent, legacy) ||
        !read_gas(bytes, length, gas_offset, extent, extended))
        return false;
    if (extended.address != 0)
    {
        if (extent != 0)
            extended.length = extent;
        if (!valid_range(extended))
            return false;
        active = extended;
    }
    else
        active = legacy;
    return add_reserved(inventory, legacy) && add_reserved(inventory, extended);
}

bool split_banks(const fixed_register &block, fixed_register &status, fixed_register &enable)
{
    status = {};
    enable = {};
    if (block.address == 0 && block.length == 0)
        return true;
    if (block.length < 2 || (block.length & 1u) != 0 || block.length / 2 > sizeof(u64) ||
        block.address > ~u64{0} - block.length / 2)
        return false;
    const u32 half = block.length / 2;
    status = block;
    status.length = half;
    enable = block;
    enable.address = block.address + half;
    enable.length = half;
    return resolve_access_width(status) && resolve_access_width(enable);
}

bool coalesce_reserved_ranges(fadt_fixed_inventory &inventory)
{
    bool merged = true;
    while (merged)
    {
        merged = false;
        for (u32 left = 0; left < inventory.reserved_count && !merged; ++left)
        {
            auto &first = inventory.reserved[left];
            const u64 first_end = first.address + first.length;
            for (u32 right = left + 1; right < inventory.reserved_count; ++right)
            {
                const auto &second = inventory.reserved[right];
                if (first.space != second.space)
                    continue;
                const u64 second_end = second.address + second.length;
                const bool overlaps =
                    first.space == 1
                        ? first.address < second_end && second.address < first_end
                        : (first.address & ~(page_size - 1)) < ((second_end + page_size - 1) & ~(page_size - 1)) &&
                              (second.address & ~(page_size - 1)) < ((first_end + page_size - 1) & ~(page_size - 1));
                if (!overlaps)
                    continue;
                const u64 base = first.address < second.address ? first.address : second.address;
                const u64 end = first_end > second_end ? first_end : second_end;
                if (end - base > ~u32{0})
                    return false;
                first = fixed_register{base, static_cast<u32>(end - base), first.space};
                for (u32 move = right + 1; move < inventory.reserved_count; ++move)
                    inventory.reserved[move - 1] = inventory.reserved[move];
                --inventory.reserved_count;
                merged = true;
                break;
            }
        }
    }
    return true;
}
} // namespace

bool parse_fadt_fixed_inventory(const u8 *bytes, u64 length, u64 boot_epoch, u64 firmware_generation,
                                fadt_fixed_inventory &result)
{
    result = {};
    if (bytes == nullptr || length < 95)
        return false;

    result.active.boot_epoch = boot_epoch;
    result.active.firmware_generation = firmware_generation;
    result.active.sci_gsi = static_cast<u16>(bytes[46]) | static_cast<u16>(bytes[47]) << 8;
    result.active.gpe1_base = bytes[94];
    u32 fadt_flags = 0;
    if (length >= 116 && !read_u32(bytes, length, 112, fadt_flags))
        return false;
    result.active.hardware_reduced = (fadt_flags & (1u << 20)) != 0;

    u32 smi_command = 0;
    if (!read_u32(bytes, length, 48, smi_command))
        return false;
    if (smi_command != 0)
    {
        result.active.smi_command = fixed_register{smi_command, 1, 1};
        if (!add_reserved(result, result.active.smi_command))
            return false;
    }

    constexpr u64 legacy_offsets[] = {56, 60, 64, 68, 72, 76, 80, 84};
    constexpr u64 gas_offsets[] = {148, 160, 172, 184, 196, 208, 220, 232};
    const u8 extents[] = {bytes[88], bytes[88], bytes[89], bytes[89], bytes[90], bytes[91], bytes[92], bytes[93]};
    fixed_register blocks[8]{};
    for (u32 index = 0; index < 8; ++index)
    {
        if (!select_block(bytes, length, legacy_offsets[index], extents[index], gas_offsets[index], blocks[index],
                          result))
            return false;
    }

    if (blocks[0].length > 2 * sizeof(u64) || blocks[1].length > 2 * sizeof(u64) || blocks[2].length > sizeof(u64) ||
        blocks[3].length > sizeof(u64) || blocks[4].length > sizeof(u64) || blocks[5].length > sizeof(u64) ||
        !resolve_access_width(blocks[2]) || !resolve_access_width(blocks[3]) || !resolve_access_width(blocks[4]) ||
        !split_banks(blocks[0], result.active.pm1_event, result.active.pm1_event_enable) ||
        !split_banks(blocks[1], result.active.pm1_event_b, result.active.pm1_event_b_enable) ||
        !split_banks(blocks[6], result.active.gpe0_status, result.active.gpe0_enable) ||
        !split_banks(blocks[7], result.active.gpe1_status, result.active.gpe1_enable))
        return false;
    result.active.pm1_control = blocks[2];
    result.active.pm1_control_b = blocks[3];
    result.active.pm2_control = blocks[4];

    if (!read_gas(bytes, length, 116, 0, result.active.reset) ||
        !read_gas(bytes, length, 244, 0, result.active.sleep_control) ||
        !read_gas(bytes, length, 256, 0, result.active.sleep_status) || !add_reserved(result, result.active.reset) ||
        !add_reserved(result, result.active.sleep_control) || !add_reserved(result, result.active.sleep_status) ||
        !coalesce_reserved_ranges(result))
        return false;

    if (length > 128)
        result.active.reset_value = bytes[128];
    return true;
}

} // namespace acpi
