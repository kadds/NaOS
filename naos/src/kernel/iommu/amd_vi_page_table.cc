#include "kernel/iommu/amd_vi_page_table.hpp"

namespace iommu::amd_vi
{
namespace
{
constexpr u64 page_mask = page_size - 1;
constexpr u64 entries_per_page = page_size / sizeof(u64);
constexpr u32 table_levels = default_page_table_levels;
constexpr u64 present = 1ull;
constexpr u64 next_level_mask = 7ull << 9;

bool range_fits_width(u64 address, u64 length, u8 width)
{
    if (length == 0 || width == 0 || address > ~u64{0} - (length - 1))
        return false;
    const u64 maximum_address = (1ull << width) - 1;
    return address <= maximum_address && length - 1 <= maximum_address - address;
}

void write_memory_barrier() { __asm__ __volatile__("mfence" : : : "memory"); }

void clear_page(void *virtual_page)
{
    auto *entries = static_cast<u64 *>(virtual_page);
    for (u64 index = 0; index < entries_per_page; ++index)
        entries[index] = 0;
}

bool flush_page(const page_table &table, void *virtual_page)
{
    return table.coherent || table.operations.flush_page(table.operations.context, virtual_page);
}

page_table_error validate_table(const page_table &table)
{
    if (table.root_virtual == nullptr || table.root_physical == 0 ||
        (reinterpret_cast<u64>(table.root_virtual) & page_mask) != 0 || (table.root_physical & page_mask) != 0 ||
        table.operations.allocate_page == nullptr || table.operations.release_page == nullptr ||
        table.operations.physical_to_virtual == nullptr || table.physical_address_width == 0 ||
        table.physical_address_width > 52 || (!table.coherent && table.operations.flush_page == nullptr))
        return page_table_error::invalid_table;
    if (table.state_uncertain)
        return page_table_error::table_state_uncertain;
    return page_table_error::none;
}

page_table_error allocate_table_page(page_table &table, void *&virtual_page, u64 &physical_address)
{
    if (!table.operations.allocate_page(table.operations.context, virtual_page, physical_address) ||
        virtual_page == nullptr)
        return page_table_error::allocation_failed;
    if ((reinterpret_cast<u64>(virtual_page) & page_mask) != 0 || (physical_address & page_mask) != 0 ||
        !range_fits_width(physical_address, page_size, table.physical_address_width) ||
        (physical_address & ~page_address_mask) != 0)
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

u32 table_index(u64 address, u32 level)
{
    const u32 shift = 12 + 9 * (level - 1);
    return static_cast<u32>((address >> shift) & 0x1ff);
}

u8 entry_next_level(u64 entry) { return static_cast<u8>((entry & next_level_mask) >> 9); }

u64 *resolve_child_table(page_table &table, u64 entry, u8 expected_level)
{
    if ((entry & present) == 0 || entry_next_level(entry) != expected_level)
        return nullptr;
    const u64 physical_address = entry & page_address_mask;
    auto *virtual_page = table.operations.physical_to_virtual(table.operations.context, physical_address);
    if (virtual_page == nullptr || (reinterpret_cast<u64>(virtual_page) & page_mask) != 0)
        return nullptr;
    return static_cast<u64 *>(virtual_page);
}

page_table_error release_table_tree(page_table &table, u64 *entries, u32 level)
{
    if (level > 1)
    {
        for (u64 index = 0; index < entries_per_page; ++index)
        {
            const u64 entry = entries[index];
            if ((entry & present) == 0)
                continue;
            auto *child = resolve_child_table(table, entry, static_cast<u8>(level - 1));
            if (child == nullptr)
                return page_table_error::address_translation_failed;
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

page_table_error create_page_table(u8 physical_address_width, bool coherent, const page_table_ops &operations,
                                   page_table &table) noexcept
{
    if (table.root_virtual != nullptr || table.root_physical != 0)
        return page_table_error::invalid_table;
    if (physical_address_width == 0 || physical_address_width > 52)
        return page_table_error::invalid_address;
    if (operations.allocate_page == nullptr || operations.release_page == nullptr ||
        operations.physical_to_virtual == nullptr)
        return page_table_error::invalid_operations;
    if (!coherent && operations.flush_page == nullptr)
        return page_table_error::cache_maintenance_unavailable;

    page_table candidate{};
    candidate.physical_address_width = physical_address_width;
    candidate.coherent = coherent;
    candidate.operations = operations;

    void *root_virtual = nullptr;
    u64 root_physical = 0;
    const auto failure = allocate_table_page(candidate, root_virtual, root_physical);
    if (failure != page_table_error::none)
        return failure;
    candidate.root_virtual = root_virtual;
    candidate.root_physical = root_physical;
    write_memory_barrier();
    table = candidate;
    return page_table_error::none;
}

page_table_error map_dma_page(page_table &table, u64 iova, u64 physical_address, dma_access access) noexcept
{
    const auto validation = validate_table(table);
    if (validation != page_table_error::none)
        return validation;
    if ((iova & page_mask) != 0 || (physical_address & page_mask) != 0 || !range_fits_width(iova, page_size, 48) ||
        !range_fits_width(physical_address, page_size, table.physical_address_width))
        return page_table_error::invalid_address;
    const u8 permissions = static_cast<u8>(access);
    if (permissions == 0 || (permissions & ~u8{3}) != 0)
        return page_table_error::invalid_permissions;

    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = table_levels; level > 1; --level)
    {
        const u32 index = table_index(iova, level);
        u64 entry = entries[index];
        if ((entry & present) == 0)
        {
            void *child_virtual = nullptr;
            u64 child_physical = 0;
            const auto allocation = allocate_table_page(table, child_virtual, child_physical);
            if (allocation != page_table_error::none)
                return allocation;
            if (!encode_page_directory_entry(child_physical, static_cast<u8>(level - 1), entry))
            {
                table.operations.release_page(table.operations.context, child_virtual);
                return page_table_error::invalid_address;
            }
            entries[index] = entry;
            write_memory_barrier();
            if (!flush_page(table, entries))
            {
                table.state_uncertain = true;
                return page_table_error::cache_maintenance_unavailable;
            }
        }

        entries = resolve_child_table(table, entry, static_cast<u8>(level - 1));
        if (entries == nullptr)
            return page_table_error::address_translation_failed;
    }

    const u32 leaf_index = table_index(iova, 1);
    if ((entries[leaf_index] & present) != 0)
        return page_table_error::already_mapped;
    u64 leaf = 0;
    if (!encode_page_translation_entry(physical_address, access, leaf))
        return page_table_error::invalid_address;
    entries[leaf_index] = leaf;
    write_memory_barrier();
    if (!flush_page(table, entries))
    {
        table.state_uncertain = true;
        return page_table_error::cache_maintenance_unavailable;
    }
    return page_table_error::none;
}

page_table_error unmap_dma_page(page_table &table, u64 iova) noexcept
{
    const auto validation = validate_table(table);
    if (validation != page_table_error::none)
        return validation;
    if ((iova & page_mask) != 0 || !range_fits_width(iova, page_size, 48))
        return page_table_error::invalid_address;

    auto *entries = static_cast<u64 *>(table.root_virtual);
    for (u32 level = table_levels; level > 1; --level)
    {
        const u64 entry = entries[table_index(iova, level)];
        if ((entry & present) == 0)
            return page_table_error::not_mapped;
        entries = resolve_child_table(table, entry, static_cast<u8>(level - 1));
        if (entries == nullptr)
            return page_table_error::address_translation_failed;
    }

    const u32 leaf_index = table_index(iova, 1);
    if ((entries[leaf_index] & present) == 0)
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

page_table_error destroy_page_table(page_table &table) noexcept
{
    if (table.root_virtual == nullptr || table.root_physical == 0 || table.operations.release_page == nullptr ||
        table.operations.physical_to_virtual == nullptr)
        return page_table_error::invalid_table;
    if (table.state_uncertain)
        return page_table_error::table_state_uncertain;
    const auto failure = release_table_tree(table, static_cast<u64 *>(table.root_virtual), table_levels);
    if (failure != page_table_error::none)
    {
        table.state_uncertain = true;
        return failure;
    }
    table = {};
    return page_table_error::none;
}
} // namespace iommu::amd_vi
