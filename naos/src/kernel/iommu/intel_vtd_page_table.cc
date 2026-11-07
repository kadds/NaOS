#include "kernel/iommu/intel_vtd_page_table.hpp"

namespace iommu::intel_vtd
{
namespace
{
constexpr u64 page_size = 4096;
constexpr u64 page_mask = page_size - 1;
constexpr u64 page_address_mask = ~page_mask;
constexpr u64 page_entry_count = page_size / sizeof(u64);
constexpr u64 permission_read = 1ull << 0;
constexpr u64 permission_write = 1ull << 1;
constexpr u64 large_page = 1ull << 7;

bool supported_address_width(u8 width) { return width == 30 || width == 39 || width == 48 || width == 57; }

bool page_entry_present(u64 entry) { return (entry & (permission_read | permission_write)) != 0; }

bool range_fits_width(u64 address, u64 length, u8 width)
{
    if (length == 0 || width == 0 || address > ~u64(0) - (length - 1))
        return false;
    if (width >= 64)
        return true;
    const u64 maximum_address = (1ull << width) - 1;
    return address <= maximum_address && length - 1 <= maximum_address - address;
}

void write_memory_barrier() { __asm__ __volatile__("mfence" : : : "memory"); }

void clear_page(void *virtual_page)
{
    auto *entries = static_cast<u64 *>(virtual_page);
    for (u64 index = 0; index < page_entry_count; ++index)
        entries[index] = 0;
}

bool flush_page(const second_level_table &table, void *virtual_page)
{
    return table.coherent || table.operations.flush_page(table.operations.context, virtual_page);
}

page_table_error allocate_table_page(second_level_table &table, void *&virtual_page, u64 &physical_address)
{
    if (!table.operations.allocate_page(table.operations.context, virtual_page, physical_address) ||
        virtual_page == nullptr)
        return page_table_error::allocation_failed;
    if ((reinterpret_cast<u64>(virtual_page) & page_mask) != 0 || (physical_address & page_mask) != 0 ||
        !range_fits_width(physical_address, page_size, table.physical_address_width))
    {
        table.operations.release_page(table.operations.context, virtual_page);
        virtual_page = nullptr;
        return page_table_error::invalid_address;
    }
    clear_page(virtual_page);
    if (!flush_page(table, virtual_page))
    {
        table.operations.release_page(table.operations.context, virtual_page);
        virtual_page = nullptr;
        return page_table_error::cache_maintenance_unavailable;
    }
    return page_table_error::none;
}

u32 table_levels(u8 address_width) { return (address_width - 12) / 9; }

u32 table_index(u64 address, u32 level)
{
    const u32 shift = 12 + 9 * (level - 1);
    return static_cast<u32>((address >> shift) & 0x1ff);
}

u64 *resolve_child_table(second_level_table &table, u64 entry)
{
    if ((entry & large_page) != 0)
        return nullptr;
    auto *virtual_page = table.operations.physical_to_virtual(table.operations.context, entry & page_address_mask);
    if (virtual_page == nullptr || (reinterpret_cast<u64>(virtual_page) & page_mask) != 0)
        return nullptr;
    return static_cast<u64 *>(virtual_page);
}

page_table_error validate_table(const second_level_table &table)
{
    if (table.root_virtual == nullptr || table.root_physical == 0 || !supported_address_width(table.address_width) ||
        table.physical_address_width == 0 || table.physical_address_width > 52 ||
        table.operations.allocate_page == nullptr || table.operations.release_page == nullptr ||
        table.operations.physical_to_virtual == nullptr || (!table.coherent && table.operations.flush_page == nullptr))
        return page_table_error::invalid_table;
    if (table.state_uncertain)
        return page_table_error::table_state_uncertain;
    return page_table_error::none;
}

page_table_error release_table_tree(second_level_table &table, u64 *entries, u32 level)
{
    if (level > 1)
    {
        for (u64 index = 0; index < page_entry_count; ++index)
        {
            const u64 entry = entries[index];
            if (!page_entry_present(entry))
                continue;
            auto *child = resolve_child_table(table, entry);
            if (child == nullptr)
                return page_table_error::page_translation_failed;
            const auto failure = release_table_tree(table, child, level - 1);
            if (failure != page_table_error::none)
                return failure;
            entries[index] = 0;
        }
    }
    table.operations.release_page(table.operations.context, entries);
    return page_table_error::none;
}
} // namespace

page_table_error create_second_level_table(u8 address_width, u8 physical_address_width, bool coherent,
                                           const page_table_ops &operations, second_level_table &table) noexcept
{
    if (table.root_virtual != nullptr || table.root_physical != 0)
        return page_table_error::invalid_table;
    if (!supported_address_width(address_width) || physical_address_width == 0 || physical_address_width > 52)
        return page_table_error::unsupported_address_width;
    if (operations.allocate_page == nullptr || operations.release_page == nullptr ||
        operations.physical_to_virtual == nullptr)
        return page_table_error::invalid_operations;
    if (!coherent && operations.flush_page == nullptr)
        return page_table_error::cache_maintenance_unavailable;

    second_level_table candidate{};
    candidate.address_width = address_width;
    candidate.physical_address_width = physical_address_width;
    candidate.coherent = coherent;
    candidate.operations = operations;

    u64 root_physical = 0;
    void *root_virtual = nullptr;
    const auto failure = allocate_table_page(candidate, root_virtual, root_physical);
    if (failure != page_table_error::none)
        return failure;
    candidate.root_virtual = root_virtual;
    candidate.root_physical = root_physical;
    write_memory_barrier();
    table = candidate;
    return page_table_error::none;
}

page_table_error map_dma_page(second_level_table &table, u64 iova, u64 physical_address, dma_access access) noexcept
{
    const auto validation = validate_table(table);
    if (validation != page_table_error::none)
        return validation;
    if ((iova & page_mask) != 0 || (physical_address & page_mask) != 0 ||
        !range_fits_width(iova, page_size, table.address_width) ||
        !range_fits_width(physical_address, page_size, table.physical_address_width))
        return page_table_error::invalid_address;

    const auto permissions = static_cast<u8>(access);
    if (permissions == 0 || (permissions & ~u8{3}) != 0)
        return page_table_error::invalid_permissions;

    const u32 levels = table_levels(table.address_width);
    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = levels; level > 1; --level)
    {
        const u32 index = table_index(iova, level);
        u64 entry = entries[index];
        if (!page_entry_present(entry))
        {
            void *child_virtual = nullptr;
            u64 child_physical = 0;
            const auto allocation = allocate_table_page(table, child_virtual, child_physical);
            if (allocation != page_table_error::none)
                return allocation;
            entries[index] = child_physical | permission_read | permission_write;
            write_memory_barrier();
            if (!flush_page(table, entries))
            {
                table.state_uncertain = true;
                return page_table_error::cache_maintenance_unavailable;
            }
            entry = entries[index];
        }
        entries = resolve_child_table(table, entry);
        if (entries == nullptr)
            return page_table_error::page_translation_failed;
    }

    const u32 leaf_index = table_index(iova, 1);
    if (page_entry_present(entries[leaf_index]))
        return page_table_error::already_mapped;
    entries[leaf_index] = physical_address | permissions;
    write_memory_barrier();
    if (!flush_page(table, entries))
    {
        table.state_uncertain = true;
        return page_table_error::cache_maintenance_unavailable;
    }
    return page_table_error::none;
}

page_table_error unmap_dma_page(second_level_table &table, u64 iova) noexcept
{
    const auto validation = validate_table(table);
    if (validation != page_table_error::none)
        return validation;
    if ((iova & page_mask) != 0 || !range_fits_width(iova, page_size, table.address_width))
        return page_table_error::invalid_address;

    const u32 levels = table_levels(table.address_width);
    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = levels; level > 1; --level)
    {
        const u64 entry = entries[table_index(iova, level)];
        if (!page_entry_present(entry))
            return page_table_error::not_mapped;
        entries = resolve_child_table(table, entry);
        if (entries == nullptr)
            return page_table_error::page_translation_failed;
    }

    const u32 leaf_index = table_index(iova, 1);
    if (!page_entry_present(entries[leaf_index]))
        return page_table_error::not_mapped;
    entries[leaf_index] = 0;
    write_memory_barrier();
    if (!flush_page(table, entries))
    {
        table.state_uncertain = true;
        return page_table_error::cache_maintenance_unavailable;
    }
    return page_table_error::none;
}

page_table_error destroy_second_level_table(second_level_table &table) noexcept
{
    if (table.root_virtual == nullptr || table.root_physical == 0 || table.operations.release_page == nullptr ||
        table.operations.physical_to_virtual == nullptr || !supported_address_width(table.address_width))
        return page_table_error::invalid_table;
    if (table.state_uncertain)
        return page_table_error::table_state_uncertain;
    const auto failure =
        release_table_tree(table, static_cast<u64 *>(table.root_virtual), table_levels(table.address_width));
    if (failure != page_table_error::none)
        return failure;
    table = {};
    return page_table_error::none;
}
} // namespace iommu::intel_vtd
