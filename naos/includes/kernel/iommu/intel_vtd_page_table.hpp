#pragma once

#include "kernel/types.hpp"

namespace iommu::intel_vtd
{
enum class page_table_error : u8
{
    none,
    invalid_table,
    invalid_operations,
    unsupported_address_width,
    invalid_address,
    invalid_permissions,
    allocation_failed,
    page_translation_failed,
    cache_maintenance_unavailable,
    already_mapped,
    not_mapped,
    table_state_uncertain,
};

enum class dma_access : u8
{
    read = 1,
    write = 2,
    read_write = 3,
};

struct page_table_ops
{
    bool (*allocate_page)(void *context, void *&virtual_page, u64 &physical_address);
    void (*release_page)(void *context, void *virtual_page);
    void *(*physical_to_virtual)(void *context, u64 physical_address);
    bool (*flush_page)(void *context, void *virtual_page);
    void *context;
};

/// Software-owned second-level page tables for one DMA domain. These routines
/// only update page-table memory; the domain owner must invalidate the IOTLB
/// before exposing new mappings, reusing an IOVA, or releasing mapped frames.
struct second_level_table
{
    void *root_virtual = nullptr;
    u64 root_physical = 0;
    u8 address_width = 0;
    u8 physical_address_width = 0;
    bool coherent = false;
    bool state_uncertain = false;
    page_table_ops operations{};
};

page_table_error create_second_level_table(u8 address_width, u8 physical_address_width, bool coherent,
                                           const page_table_ops &operations, second_level_table &table) noexcept;
page_table_error map_dma_page(second_level_table &table, u64 iova, u64 physical_address, dma_access access) noexcept;
page_table_error unmap_dma_page(second_level_table &table, u64 iova) noexcept;

/// The caller must have removed all device contexts and completed context and
/// IOTLB invalidation before destroying a table that was ever hardware-visible.
page_table_error destroy_second_level_table(second_level_table &table) noexcept;
} // namespace iommu::intel_vtd
